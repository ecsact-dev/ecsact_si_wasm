#include "ecsactsi_wasm.h"

#include <map>
#include <unordered_map>
#include <mutex>
#include <variant>
#include <vector>
#include <string>
#include <string_view>
#include <shared_mutex>
#include <cassert>
#include <wasm.h>
#include <wasmer.h>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <iostream>
#include "ecsact/runtime/dynamic.h"
#include "ecsact/runtime/meta.h"

#include "wasm_ecsact_system_execution.h"
#include "wasm_ecsact_pointer_map.hh"
#include "wasm_ecsact_memory.hh"
#include "ecsactsi_wasi.hh"
#include "ecsactsi_logger.hh"
#include "wasm_file_binary.hh"
#include "wasm_util.hh"
#include "guest_imports.hh"
#include "guest_imports/env.hh"
#include "guest_imports/wasi_snapshot_preview1.hh"
#include "ecsactsi_wasm_internal.hh"

using namespace std::string_literals;
using ecsactsi_wasm::detail::allowed_guest_modules_t;
using ecsactsi_wasm::detail::guest_env_module_imports;
using ecsactsi_wasm::detail::guest_wasi_module_imports;
using ecsactsi_wasm::detail::wasm_file_binary;
using ecsactsi_wasm::detail::wasm_functype_new_4_0;
using ecsactsi_wasm::detail::wasm_functype_new_4_1;

namespace {
auto modules_mutex = std::shared_mutex{};
auto modules =
	std::map<ecsact_system_like_id, ecsact_internal_wasm_system_module_info>{};
auto trap_handler = ecsactsi_wasm_trap_handler{};

void unload_system_module(
	ecsact_system_like_id                    sys_id,
	ecsact_internal_wasm_system_module_info& info
) {
	ecsact_set_system_execution_impl(sys_id, nullptr);
	wasm_module_delete(info.system_module);
	wasm_instance_delete(info.instance);
	wasm_func_delete(info.system_impl_func);
	wasm_memory_delete(info.system_impl_memory);
	wasm_store_delete(info.store);

	info = {};
}

void ecsactsi_wasm_system_impl(ecsact_system_execution_context* ctx) {
	auto lk = std::shared_lock(modules_mutex);
	auto system_id = ecsact_system_execution_context_id(ctx);
	auto info = get_ecsact_internal_module_info(system_id);

	set_wasm_ecsact_system_execution_context_memory(
		ctx,
		info->system_impl_memory
	);

	wasm_val_t as[1] = {{}};
	as[0].kind = WASM_I32;
	as[0].of.i32 = ecsactsi_wasm::as_guest_pointer(ctx);

	wasm_val_vec_t args = WASM_ARRAY_VEC(as);
	wasm_val_vec_t results = WASM_EMPTY_VEC;

	ecsactsi_wasm::detail::set_current_wasm_memory(info->system_impl_memory);

	wasm_trap_t* trap = wasm_func_call(info->system_impl_func, &args, &results);

	ecsactsi_wasm::detail::set_current_wasm_memory(nullptr);

	if(trap_handler != nullptr && trap != nullptr) {
		wasm_message_t trap_msg;
		wasm_trap_message(trap, &trap_msg);
		auto trap_msg_str = std::string{trap_msg.data, trap_msg.size - 1};
		auto origin = wasm_trap_origin(trap);

		if(origin != nullptr) {
			auto fn_index = wasm_frame_func_index(origin);
			auto fn_offset = wasm_frame_func_offset(origin);

			trap_msg_str += " (func_index=" + std::to_string(fn_index) + ", ";
			trap_msg_str += "func_offset=" + std::to_string(fn_offset) + ")";
			wasm_frame_delete(origin);
		} else {
			trap_msg_str += " (unknown trap origin)";
		}

		trap_handler(system_id, trap_msg_str.c_str());
	}

	if(info->parent) {
		set_wasm_ecsact_system_execution_context_memory(
			const_cast<ecsact_system_execution_context*>(info->parent),
			nullptr
		);
	}

	auto other_ll = info->other_contexts;
	while(other_ll) {
		set_wasm_ecsact_system_execution_context_memory(other_ll->ctx, nullptr);
		auto next = other_ll->next;
		delete other_ll;
		other_ll = next;
	}

	info->other_contexts = nullptr;
	info->parent = nullptr;

	set_wasm_ecsact_system_execution_context_memory(ctx, nullptr);
}
} // namespace

void ecsactsi_wasm_last_error_message(
	char*   out_message,
	int32_t message_max_length
) {
	auto& last_error_message = ecsactsi_wasm::detail::get_last_error_message();
	std::copy_n(
		last_error_message.begin(),
		std::min(
			message_max_length,
			static_cast<int32_t>(last_error_message.size())
		),
		out_message
	);
}

int32_t ecsactsi_wasm_last_error_message_length() {
	return static_cast<int32_t>(
		ecsactsi_wasm::detail::get_last_error_message().size()
	);
}

ecsact_internal_wasm_system_module_info* get_ecsact_internal_module_info(
	ecsact_system_like_id sys_id
) {
	assert(modules.contains(sys_id));
	return &modules.at(sys_id);
}

ecsactsi_wasm_error ecsactsi_wasm_load(
	char*                  wasm_data,
	int                    wasm_data_size,
	int                    systems_count,
	ecsact_system_like_id* system_ids,
	const char**           wasm_exports
) {
	auto load_result = ecsactsi_wasm::detail::load_modules(
		wasm_data,
		wasm_data_size,
		systems_count,
		system_ids,
		wasm_exports
	);

	if(std::holds_alternative<ecsactsi_wasm_error>(load_result)) {
		return std::get<ecsactsi_wasm_error>(load_result);
	}

	auto& pending_modules = std::get<1>(load_result);

	for(auto&& [system_id, pending_info] : pending_modules) {
		auto trap = ecsactsi_wasm::detail::init_module(pending_info);
		if(trap && trap_handler) {
			auto trap_msg = ecsactsi_wasm::detail::trap_message(trap);
			trap_handler(static_cast<ecsact_system_like_id>(-1), trap_msg.c_str());
			return ECSACTSI_WASM_ERR_INSTANTIATE_FAIL;
		}
	}

	std::unique_lock lk(modules_mutex);
	for(auto&& [system_id, pending_info] : pending_modules) {
		if(modules.contains(system_id)) {
			unload_system_module(system_id, modules.at(system_id));
		}

		modules[system_id] = std::move(pending_info);
		ecsact_set_system_execution_impl(system_id, &ecsactsi_wasm_system_impl);
	}
	lk.unlock();

	return ECSACTSI_WASM_OK;
}

ecsactsi_wasm_error ecsactsi_wasm_load_file(
	const char*            wasm_file_path,
	int                    systems_count,
	ecsact_system_like_id* system_ids,
	const char**           wasm_exports
) {
	FILE* file = std::fopen(wasm_file_path, "rb");
	if(!file) {
		return ECSACTSI_WASM_ERR_FILE_OPEN_FAIL;
	}

	wasm_file_binary file_bin;
	{
		const bool read_success = wasm_file_binary::read(file, file_bin);
		std::fclose(file);
		if(!read_success) {
			return ECSACTSI_WASM_ERR_FILE_READ_FAIL;
		}
	}

	return ecsactsi_wasm_load(
		file_bin.binary.data,
		file_bin.binary.size,
		systems_count,
		system_ids,
		wasm_exports
	);
}

void ecsactsi_wasm_set_trap_handler(ecsactsi_wasm_trap_handler handler) {
	std::shared_lock lk(modules_mutex);
	trap_handler = handler;
}

void ecsactsi_wasm_unload(
	int                    systems_count,
	ecsact_system_like_id* system_ids
) {
	std::unique_lock lk(modules_mutex);
	for(int i = 0; systems_count > i; ++i) {
		auto system_id = system_ids[i];
		auto module_itr = modules.find(system_id);
		if(module_itr != modules.end()) {
			unload_system_module(system_id, module_itr->second);
			modules.erase(module_itr);
		}
	}
}

void ecsactsi_wasm_reset() {
	std::unique_lock lk(modules_mutex);
	for(auto&& [sys_id, m] : modules) {
		unload_system_module(sys_id, m);
	}
	modules.clear();
	trap_handler = nullptr;
}

void ecsactsi_wasm_consume_logs(
	ecsactsi_wasm_log_consumer consumer,
	void*                      consumer_user_data
) {
	auto t = ecsactsi_wasm::detail::start_transaction();
	for(auto& entry : ecsactsi_wasm::detail::get_log_lines(t)) {
		consumer(
			entry.log_level,
			entry.message.c_str(),
			static_cast<int32_t>(entry.message.size()),
			consumer_user_data
		);
	}
	ecsactsi_wasm::detail::clear_log_lines(t);
}