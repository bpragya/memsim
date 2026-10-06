// Runs physical addresses through Ramulator2's own channel mapper + controller
// address mapper (from a YAML config) and prints the resulting address vector.
// usage: map_check <config.yaml> <num_random>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/channel_mapper/i_channel_mapper.h"
#include "ramulator/memory_system/i_memory_system.h"

struct ImplChildren : Ramulator::Implementation {
  static std::vector<std::unique_ptr<Ramulator::Implementation>>& of(Ramulator::Implementation* impl) {
    return impl->*(&ImplChildren::m_children);
  }
};

int main(int argc, char** argv) {
  auto config = Ramulator::Config::parse_config_file(argv[1]);
  long n = atol(argv[2]);
  auto* fe = Ramulator::Factory::create_frontend(config);
  auto* ms = Ramulator::Factory::create_memory_system(config);
  fe->connect_memory_system(ms);
  ms->connect_frontend(fe);

  Ramulator::IChannelMapper* chmap = nullptr;
  std::vector<Ramulator::ControllerBase*> ctrls;
  for (auto& c : ImplChildren::of(dynamic_cast<Ramulator::Implementation*>(ms))) {
    if (auto* m = dynamic_cast<Ramulator::IChannelMapper*>(c.get())) chmap = m;
    if (auto* k = dynamic_cast<Ramulator::ControllerBase*>(c.get())) ctrls.push_back(k);
  }
  const auto& spec = *ctrls[0]->m_device.m_spec;
  printf("# levels:");
  for (auto& name : spec.level_names) printf(" %s", name.c_str());
  printf("\n");

  std::mt19937_64 rng(1234);
  const unsigned long long lines = 1ULL << 29;  // 32GB of 64B lines
  for (long i = 0; i < n; i++) {
    unsigned long long line = (i < 4096) ? (unsigned long long)i : rng() % lines;
    Ramulator::Request req((Ramulator::Addr_t)(line << 6), Ramulator::Request::Type::Read);
    chmap->apply(req);
    ctrls[req.addr_vec[0]]->m_addr_mapper->apply(req);
    printf("%llu", line << 6);
    for (auto v : req.addr_vec) printf(" %d", v);
    printf("\n");
  }
  return 0;
}
