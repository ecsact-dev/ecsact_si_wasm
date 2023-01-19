#include "ecsactsi_wasi.hh"

#include <cstdio>
#include <map>
#include <string_view>

#include "wasm_ecsact_memory.hh"
#include "ecsactsi_logger.hh"

constexpr int32_t WASI_STDIN_FD = 0;
constexpr int32_t WASI_STDOUT_FD = 1;
constexpr int32_t WASI_STDERR_FD = 2;

wasm_trap_t* ecsactsi_wasi_proc_exit(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_fd_seek(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	results->data[0].kind = WASM_I32;
	results->data[0].of.i32 = 0;

	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_fd_write(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	auto mem = ecsactsi_wasm::current_wasm_memory_rw();
	assert(args->data[0].kind == WASM_I32);
	auto fd = args->data[0].of.i32;

	assert(args->data[1].kind == WASM_I32);
	auto iovec = ecsactsi_wasm::wasm_memory_cast<ecsactsi_wasi_ciovec_t>(
		mem,
		args->data[1].of.i32
	);

	assert(args->data[2].kind == WASM_I32);
	auto iovec_len = args->data[2].of.i32;

	assert(args->data[3].kind == WASM_I32);
	auto out_write_amount =
		ecsactsi_wasm::wasm_memory_cast<size_t>(mem, args->data[3].of.i32);

	// Only supporting stdout and stderr for now
	assert(fd == 1 || fd == 2);

	auto f = fd == 1 ? stdout : stderr;

	auto write_amount = size_t{};

	for(int i = 0; iovec_len > i; ++i) {
		auto io = iovec[i];

		if(io.buf_len > 0) {
			auto buf = ecsactsi_wasm::wasm_memory_cast<const char>(mem, io.buf);
			auto str = std::string_view(buf, io.buf_len);
			write_amount += io.buf_len;

			std::fprintf(f, "%.*s", static_cast<int>(str.size()), str.data());
		}
	}

	*out_write_amount = write_amount;

	results->data[0].kind = WASM_I32;
	results->data[0].of.i32 = 0;

	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_fd_read(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	results->data[0].kind = WASM_I32;
	results->data[0].of.i32 = 1;

	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_fd_close(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	results->data[0].kind = WASM_I32;
	results->data[0].of.i32 = 0;

	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_environ_sizes_get(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	auto mem = ecsactsi_wasm::current_wasm_memory_rw();
	auto retptr0 =
		ecsactsi_wasm::wasm_memory_cast<size_t>(mem, args->data[3].of.i32);
	auto retptr1 =
		ecsactsi_wasm::wasm_memory_cast<size_t>(mem, args->data[3].of.i32);

	*retptr0 = 0;
	*retptr1 = 0;

	results->data[0].kind = WASM_I32;
	results->data[0].of.i32 = 0;

	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_environ_get(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	auto mem = ecsactsi_wasm::current_wasm_memory_rw();
	auto environ_arg =
		ecsactsi_wasm::wasm_memory_cast<uint8_t>(mem, args->data[0].of.i32);
	auto environ_buf_arg =
		ecsactsi_wasm::wasm_memory_cast<uint8_t>(mem, args->data[1].of.i32);

	// Even though our environment variable size is `0` the API expects the key
	// values pairs to be null terminated.
	environ_arg[0] = '\0';
	environ_buf_arg[0] = '\0';

	results->data[0].kind = WASM_I32;
	results->data[0].of.i32 = 0;

	return nullptr;
}

wasm_trap_t* ecsactsi_wasi_fd_fdstat_get(
	const wasm_val_vec_t* args,
	wasm_val_vec_t*       results
) {
	const auto default_fdstats = std::map<int32_t, ecsactsi_wasi_fdstat_t>{
		{
			WASI_STDIN_FD,
			ecsactsi_wasi_fdstat_t{},
		},
		{
			WASI_STDOUT_FD,
			ecsactsi_wasi_fdstat_t{
				.fs_filetype = ecsactsi_wasi_filetype::character_device,
				.fs_flags = {},
				.fs_rights_base = ecsactsi_wasi_rights::fd_datasync |
					ecsactsi_wasi_rights::fd_sync | ecsactsi_wasi_rights::fd_write |
					ecsactsi_wasi_rights::fd_advise |
					ecsactsi_wasi_rights::fd_filestat_get |
					ecsactsi_wasi_rights::poll_fd_readwrite,
				.fs_rights_inheriting = {},
			},
		},
		{
			WASI_STDERR_FD,
			ecsactsi_wasi_fdstat_t{
				.fs_filetype = ecsactsi_wasi_filetype::character_device,
				.fs_flags = {},
				.fs_rights_base = ecsactsi_wasi_rights::fd_datasync |
					ecsactsi_wasi_rights::fd_sync | ecsactsi_wasi_rights::fd_write |
					ecsactsi_wasi_rights::fd_advise |
					ecsactsi_wasi_rights::fd_filestat_get |
					ecsactsi_wasi_rights::poll_fd_readwrite,
				.fs_rights_inheriting = {},
			},
		},
	};

	auto mem = ecsactsi_wasm::current_wasm_memory_rw();

	assert(args->data[0].kind == WASM_I32);
	auto fd = args->data[0].of.i32;

	assert(args->data[1].kind == WASM_I32);
	auto ret = ecsactsi_wasm::wasm_memory_cast<ecsactsi_wasi_fdstat_t>(
		mem,
		args->data[1].of.i32
	);

	if(default_fdstats.contains(fd)) {
		*ret = default_fdstats.at(fd);
	} else {
		ret->fs_filetype = {};
		ret->fs_flags = {};
		ret->fs_rights_base = {};
		ret->fs_rights_inheriting = {};
	}

	if(ret->fs_filetype != ecsactsi_wasi_filetype::unknown) {
		results->data[0].kind = WASM_I32;
		results->data[0].of.i32 = 0;
	} else {
		results->data[0].kind = WASM_I32;
		results->data[0].of.i32 = 1;
	}

	return nullptr;
}