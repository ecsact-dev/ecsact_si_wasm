#include "example.ecsact.hh"
#include "example.ecsact.systems.hh"

void example::ExampleSystem::impl(context& ctx) {
	auto comp = ctx.get<example::ExampleComponent>();
	comp.num += 1;
	ctx.update(comp);
}
