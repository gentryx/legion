/* Copyright 2015 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <time.h>

#include "circuit.h"
#include "circuit_mapper.h"
#include "legion.h"

using namespace LegionRuntime::HighLevel;
using namespace LegionRuntime::Accessor;

LegionRuntime::Logger::Category log_circuit("circuit");

// Utility functions (forward declarations)
void parse_input_args(char **argv, int argc, int &num_loops, int &num_pieces,
                      int &nodes_per_piece, int &wires_per_piece,
                      int &pct_wire_in_piece, int &random_seed,
                      int &steps, int &sync, bool &perform_checks, bool &dump_values);

Partitions load_circuit(Circuit &ckt, std::vector<CircuitPiece> &pieces, Context ctx,
                        HighLevelRuntime *runtime, int num_pieces, int nodes_per_piece,
                        int wires_per_piece, int pct_wire_in_piece, int random_seed,
			int steps);

void allocate_node_fields(Context ctx, HighLevelRuntime *runtime, FieldSpace node_space);
void allocate_wire_fields(Context ctx, HighLevelRuntime *runtime, FieldSpace wire_space);
void allocate_locator_fields(Context ctx, HighLevelRuntime *runtime, FieldSpace locator_space);

void top_level_task(const Task *task,
                    const std::vector<PhysicalRegion> &regions,
                    Context ctx, HighLevelRuntime *runtime)
{
  int num_loops = 2;
  int num_pieces = 4;
  int nodes_per_piece = 2;
  int wires_per_piece = 4;
  int pct_wire_in_piece = 95;
  int random_seed = 12345;
  int steps = STEPS;
  int sync = 0;
  bool perform_checks = false;
  bool dump_values = false;
  {
    const InputArgs &command_args = HighLevelRuntime::get_input_args();
    char **argv = command_args.argv;
    int argc = command_args.argc;

    parse_input_args(argv, argc, num_loops, num_pieces, nodes_per_piece, 
		     wires_per_piece, pct_wire_in_piece, random_seed,
		     steps, sync, perform_checks, dump_values);

    log_circuit.print("circuit settings: loops=%d pieces=%d nodes/piece=%d "
                            "wires/piece=%d pct_in_piece=%d seed=%d",
       num_loops, num_pieces, nodes_per_piece, wires_per_piece,
       pct_wire_in_piece, random_seed);
  }

  Circuit circuit;
  {
    int num_circuit_nodes = num_pieces * nodes_per_piece;
    int num_circuit_wires = num_pieces * wires_per_piece;
    // Make index spaces
    IndexSpace node_index_space = runtime->create_index_space(ctx,num_circuit_nodes);
    runtime->attach_name(node_index_space, "NODE INDEX SPACE");
    IndexSpace wire_index_space = runtime->create_index_space(ctx,num_circuit_wires);
    runtime->attach_name(wire_index_space, "WIRE INDEX SPACE");
    // Make field spaces
    FieldSpace node_field_space = runtime->create_field_space(ctx);
    runtime->attach_name(node_field_space, "NODE FIELD SPACE");
    FieldSpace wire_field_space = runtime->create_field_space(ctx);
    runtime->attach_name(wire_field_space, "WIRE FIELD SPACE");
    FieldSpace locator_field_space = runtime->create_field_space(ctx);
    runtime->attach_name(locator_field_space, "LOCATOR FIELD SPACE");
    // Allocate fields
    allocate_node_fields(ctx, runtime, node_field_space);
    allocate_wire_fields(ctx, runtime, wire_field_space);
    allocate_locator_fields(ctx, runtime, locator_field_space);
    // Make logical regions
    circuit.all_nodes = runtime->create_logical_region(ctx,node_index_space,node_field_space);
    runtime->attach_name(circuit.all_nodes, "ALL NODES");
    circuit.all_wires = runtime->create_logical_region(ctx,wire_index_space,wire_field_space);
    runtime->attach_name(circuit.all_wires, "ALL WIRES");
    circuit.node_locator = runtime->create_logical_region(ctx,node_index_space,locator_field_space);
    runtime->attach_name(circuit.node_locator, "NODE LOCATOR");
  }

  // Load the circuit
  std::vector<CircuitPiece> pieces(num_pieces);
  Partitions parts = load_circuit(circuit, pieces, ctx, runtime, num_pieces, nodes_per_piece,
                                  wires_per_piece, pct_wire_in_piece, random_seed, steps);

  // Arguments for each point
  ArgumentMap local_args;
  for (int idx = 0; idx < num_pieces; idx++)
  {
    DomainPoint point = DomainPoint::from_point<1>(Point<1>(idx));
    local_args.set_point(point, TaskArgument(&(pieces[idx]),sizeof(CircuitPiece)));
  }

  // Make the launchers
  Rect<1> launch_rect(Point<1>(0), Point<1>(num_pieces-1)); 
  Domain launch_domain = Domain::from_rect<1>(launch_rect);
  CalcNewCurrentsTask cnc_launcher(parts.pvt_wires, parts.pvt_nodes, parts.shr_nodes, parts.ghost_nodes,
                                   circuit.all_wires, circuit.all_nodes, launch_domain, local_args);

  DistributeChargeTask dsc_launcher(parts.pvt_wires, parts.pvt_nodes, parts.shr_nodes, parts.ghost_nodes,
                                    circuit.all_wires, circuit.all_nodes, launch_domain, local_args);

  UpdateVoltagesTask upv_launcher(parts.pvt_nodes, parts.shr_nodes, parts.node_locations,
                                 circuit.all_nodes, circuit.node_locator, launch_domain, local_args);

  printf("Starting main simulation loop\n");
  //struct timespec ts_start, ts_end;
  //clock_gettime(CLOCK_MONOTONIC, &ts_start);
  double ts_start, ts_end;
  ts_start = Realm::Clock::current_time_in_microseconds();
  // Run the main loop
  bool simulation_success = true;
  for (int i = 0; i < num_loops; i++)
  {
    TaskHelper::dispatch_task<CalcNewCurrentsTask>(cnc_launcher, ctx, runtime, 
                                                   perform_checks, simulation_success);
    TaskHelper::dispatch_task<DistributeChargeTask>(dsc_launcher, ctx, runtime, 
                                                    perform_checks, simulation_success);
    TaskHelper::dispatch_task<UpdateVoltagesTask>(upv_launcher, ctx, runtime, 
                                                  perform_checks, simulation_success,
                                                  ((i+1)==num_loops));
  }
  ts_end = Realm::Clock::current_time_in_microseconds();
  if (simulation_success)
    printf("SUCCESS!\n");
  else
    printf("FAILURE!\n");
  {
    double sim_time = 1e-6 * (ts_end - ts_start);
    printf("ELAPSED TIME = %7.3f s\n", sim_time);

    // Compute the floating point operations per second
    long num_circuit_nodes = num_pieces * nodes_per_piece;
    long num_circuit_wires = num_pieces * wires_per_piece;
    // calculate currents
    long operations = num_circuit_wires * (WIRE_SEGMENTS*6 + (WIRE_SEGMENTS-1)*4) * steps;
    // distribute charge
    operations += (num_circuit_wires * 4);
    // update voltages
    operations += (num_circuit_nodes * 4);
    // multiply by the number of loops
    operations *= num_loops;

    // Compute the number of gflops
    double gflops = (1e-9*operations)/sim_time;
    printf("GFLOPS = %7.3f GFLOPS\n", gflops);
  }
  log_circuit.print("simulation complete - destroying regions");

  if (dump_values)
  {
    RegionRequirement wires_req(circuit.all_wires, READ_ONLY, EXCLUSIVE, circuit.all_wires);
    for (int i = 0; i < WIRE_SEGMENTS; i++)
      wires_req.add_field(FID_CURRENT+i);
    for (int i = 0; i < (WIRE_SEGMENTS-1); i++)
      wires_req.add_field(FID_WIRE_VOLTAGE+i);
    PhysicalRegion wires = runtime->map_region(ctx, wires_req);
    wires.wait_until_valid();
    RegionAccessor<AccessorType::Generic, float> fa_wire_currents[WIRE_SEGMENTS];
    for (int i = 0; i < WIRE_SEGMENTS; i++)
      fa_wire_currents[i] = wires.get_field_accessor(FID_CURRENT+i).typeify<float>();
    RegionAccessor<AccessorType::Generic, float> fa_wire_voltages[WIRE_SEGMENTS-1];
    for (int i = 0; i < (WIRE_SEGMENTS-1); i++)
      fa_wire_voltages[i] = wires.get_field_accessor(FID_WIRE_VOLTAGE+i).typeify<float>();
    IndexIterator itr(runtime, ctx, circuit.all_wires.get_index_space());
    while (itr.has_next())
    {
      ptr_t wire_ptr = itr.next();
      for (int i = 0; i < WIRE_SEGMENTS; ++i)
        printf(" %.5g", fa_wire_currents[i].read(wire_ptr));
      for (int i = 0; i < WIRE_SEGMENTS - 1; ++i)
        printf(" %.5g", fa_wire_voltages[i].read(wire_ptr));
      printf("\n");
    }
    runtime->unmap_region(ctx, wires);
  }

  // Now we can destroy all the things that we made
  {
    runtime->destroy_logical_region(ctx,circuit.all_nodes);
    runtime->destroy_logical_region(ctx,circuit.all_wires);
    runtime->destroy_logical_region(ctx,circuit.node_locator);
    runtime->destroy_field_space(ctx,circuit.all_nodes.get_field_space());
    runtime->destroy_field_space(ctx,circuit.all_wires.get_field_space());
    runtime->destroy_field_space(ctx,circuit.node_locator.get_field_space());
    runtime->destroy_index_space(ctx,circuit.all_nodes.get_index_space());
    runtime->destroy_index_space(ctx,circuit.all_wires.get_index_space());
  }
}

static void update_mappers(Machine machine, HighLevelRuntime *rt,
                           const std::set<Processor> &local_procs)
{
  for (std::set<Processor>::const_iterator it = local_procs.begin();
        it != local_procs.end(); it++)
  {
    rt->replace_default_mapper(new CircuitMapper(machine, rt, *it), *it);
  }
}

int main(int argc, char **argv)
{
  HighLevelRuntime::set_top_level_task_id(TOP_LEVEL_TASK_ID);
  HighLevelRuntime::register_legion_task<top_level_task>(TOP_LEVEL_TASK_ID,
      Processor::LOC_PROC, true/*single*/, false/*index*/,
      AUTO_GENERATE_ID, TaskConfigOptions(), "top_level");
  // If we're running on the shared low-level then only register cpu tasks
#ifdef SHARED_LOWLEVEL
  TaskHelper::register_cpu_variants<CalcNewCurrentsTask>();
  TaskHelper::register_cpu_variants<DistributeChargeTask>();
  TaskHelper::register_cpu_variants<UpdateVoltagesTask>();
#else
  TaskHelper::register_hybrid_variants<CalcNewCurrentsTask>();
  TaskHelper::register_hybrid_variants<DistributeChargeTask>();
  TaskHelper::register_hybrid_variants<UpdateVoltagesTask>();
#endif
  CheckTask::register_task();
  HighLevelRuntime::register_reduction_op<AccumulateCharge>(REDUCE_ID);
  HighLevelRuntime::set_registration_callback(update_mappers);

  return HighLevelRuntime::start(argc, argv);
}

void parse_input_args(char **argv, int argc, int &num_loops, int &num_pieces,
                      int &nodes_per_piece, int &wires_per_piece,
                      int &pct_wire_in_piece, int &random_seed,
                      int &steps, int &sync, bool &perform_checks,
                      bool &dump_values)
{
  for (int i = 1; i < argc; i++) 
  {
    if (!strcmp(argv[i], "-l")) 
    {
      num_loops = atoi(argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-i")) 
    {
      steps = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-p")) 
    {
      num_pieces = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-npp")) 
    {
      nodes_per_piece = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-wpp")) 
    {
      wires_per_piece = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-pct")) 
    {
      pct_wire_in_piece = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-s")) 
    {
      random_seed = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-sync")) 
    {
      sync = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-checks"))
    {
      perform_checks = true;
      continue;
    }

    if(!strcmp(argv[i], "-dump"))
    {
      dump_values = true;
      continue;
    }
  }
}

void allocate_node_fields(Context ctx, HighLevelRuntime *runtime, FieldSpace node_space)
{
  FieldAllocator allocator = runtime->create_field_allocator(ctx, node_space);
  allocator.allocate_field(sizeof(float), FID_NODE_CAP);
  runtime->attach_name(node_space, FID_NODE_CAP, "NODE CAPACITANCE");
  allocator.allocate_field(sizeof(float), FID_LEAKAGE);
  runtime->attach_name(node_space, FID_LEAKAGE, "LEAKAGE");
  allocator.allocate_field(sizeof(float), FID_CHARGE);
  runtime->attach_name(node_space, FID_CHARGE, "CHARGE");
  allocator.allocate_field(sizeof(float), FID_NODE_VOLTAGE);
  runtime->attach_name(node_space, FID_NODE_VOLTAGE, "NODE VOLTAGE");
  allocator.allocate_field(sizeof(Color), FID_NODE_COLOR);
  runtime->attach_name(node_space, FID_NODE_COLOR, "NODE COLOR");
}

void allocate_wire_fields(Context ctx, HighLevelRuntime *runtime, FieldSpace wire_space)
{
  FieldAllocator allocator = runtime->create_field_allocator(ctx, wire_space);
  allocator.allocate_field(sizeof(ptr_t), FID_IN_PTR);
  runtime->attach_name(wire_space, FID_IN_PTR, "IN PTR");
  allocator.allocate_field(sizeof(ptr_t), FID_OUT_PTR);
  runtime->attach_name(wire_space, FID_OUT_PTR, "OUT PTR");
  allocator.allocate_field(sizeof(PointerLocation), FID_IN_LOC);
  runtime->attach_name(wire_space, FID_IN_LOC, "IN LOC");
  allocator.allocate_field(sizeof(PointerLocation), FID_OUT_LOC);
  runtime->attach_name(wire_space, FID_OUT_LOC, "OUT LOC");
  allocator.allocate_field(sizeof(float), FID_INDUCTANCE);
  runtime->attach_name(wire_space, FID_INDUCTANCE, "INDUCTANCE");
  allocator.allocate_field(sizeof(float), FID_RESISTANCE);
  runtime->attach_name(wire_space, FID_RESISTANCE, "RESISTANCE");
  allocator.allocate_field(sizeof(float), FID_WIRE_CAP);
  runtime->attach_name(wire_space, FID_WIRE_CAP, "WIRE CAPACITANCE");
  for (int i = 0; i < WIRE_SEGMENTS; i++)
  {
    char field_name[10];
    allocator.allocate_field(sizeof(float), FID_CURRENT+i);
    sprintf(field_name, "CURRENT %d", i);
    runtime->attach_name(wire_space, FID_CURRENT+i, field_name);
  }
  for (int i = 0; i < (WIRE_SEGMENTS-1); i++)
  {
    char field_name[15];
    allocator.allocate_field(sizeof(float), FID_WIRE_VOLTAGE+i);
    sprintf(field_name, "WIRE VOLTAGE %d", i);
    runtime->attach_name(wire_space, FID_WIRE_VOLTAGE+i, field_name);
  }
}

void allocate_locator_fields(Context ctx, HighLevelRuntime *runtime, FieldSpace locator_space)
{
  FieldAllocator allocator = runtime->create_field_allocator(ctx, locator_space);
  allocator.allocate_field(sizeof(PointerLocation), FID_LOCATOR);
  runtime->attach_name(locator_space, FID_LOCATOR, "LOCATOR");
}

PointerLocation find_location(ptr_t ptr, HighLevelRuntime *runtime, Context ctx,
                              LogicalRegion pvt_nodes, LogicalRegion shr_nodes,
                              LogicalRegion ghost_nodes)
{
  ptr_t pvt_ptr = runtime->safe_cast(ctx, ptr, pvt_nodes);
  ptr_t shr_ptr = runtime->safe_cast(ctx, ptr, shr_nodes);
  ptr_t ghost_ptr = runtime->safe_cast(ctx, ptr, ghost_nodes);
  // Make sure exactly one of these is non-null
  assert((!pvt_ptr.is_null() && shr_ptr.is_null() && ghost_ptr.is_null()) ||
         (!shr_ptr.is_null() && pvt_ptr.is_null() && ghost_ptr.is_null()) ||
         (!ghost_ptr.is_null() && pvt_ptr.is_null() && shr_ptr.is_null()));
  if (!pvt_ptr.is_null())
    return PRIVATE_PTR;
  if (!shr_ptr.is_null())
    return SHARED_PTR;
  return GHOST_PTR;
}

template<typename T>
static T random_element(const std::set<T> &set)
{
  int index = int(drand48() * set.size());
  typename std::set<T>::const_iterator it = set.begin();
  while (index-- > 0) it++;
  return *it;
}

template<typename T>
static T random_element(const std::vector<T> &vec)
{
  int index = int(drand48() * vec.size());
  return vec[index];
}

Partitions load_circuit(Circuit &ckt, std::vector<CircuitPiece> &pieces, Context ctx,
                        HighLevelRuntime *runtime, int num_pieces, int nodes_per_piece,
                        int wires_per_piece, int pct_wire_in_piece, int random_seed,
			int steps)
{
  log_circuit.print("Initializing circuit simulation...");
  // inline map physical instances for the nodes and wire regions
  RegionRequirement wires_req(ckt.all_wires, READ_WRITE, EXCLUSIVE, ckt.all_wires);
  wires_req.add_field(FID_IN_PTR);
  wires_req.add_field(FID_OUT_PTR);
  wires_req.add_field(FID_IN_LOC);
  wires_req.add_field(FID_OUT_LOC);
  wires_req.add_field(FID_INDUCTANCE);
  wires_req.add_field(FID_RESISTANCE);
  wires_req.add_field(FID_WIRE_CAP);
  for (int i = 0; i < WIRE_SEGMENTS; i++)
    wires_req.add_field(FID_CURRENT+i);
  for (int i = 0; i < (WIRE_SEGMENTS-1); i++)
    wires_req.add_field(FID_WIRE_VOLTAGE+i);
  RegionRequirement nodes_req(ckt.all_nodes, READ_WRITE, EXCLUSIVE, ckt.all_nodes);
  nodes_req.add_field(FID_NODE_CAP);
  nodes_req.add_field(FID_LEAKAGE);
  nodes_req.add_field(FID_CHARGE);
  nodes_req.add_field(FID_NODE_VOLTAGE);
  nodes_req.add_field(FID_NODE_COLOR);
  PhysicalRegion wires = runtime->map_region(ctx, wires_req);
  PhysicalRegion nodes = runtime->map_region(ctx, nodes_req);

  // keep a O(1) indexable list of nodes in each piece for connecting wires
  std::vector<std::vector<ptr_t> > piece_node_ptrs(num_pieces);
  std::vector<int> piece_shared_nodes(num_pieces, 0);

  srand48(random_seed);

  nodes.wait_until_valid();
  RegionAccessor<AccessorType::Generic, float> fa_node_cap = 
    nodes.get_field_accessor(FID_NODE_CAP).typeify<float>();
  RegionAccessor<AccessorType::Generic, float> fa_node_leakage = 
    nodes.get_field_accessor(FID_LEAKAGE).typeify<float>();
  RegionAccessor<AccessorType::Generic, float> fa_node_charge = 
    nodes.get_field_accessor(FID_CHARGE).typeify<float>();
  RegionAccessor<AccessorType::Generic, float> fa_node_voltage = 
    nodes.get_field_accessor(FID_NODE_VOLTAGE).typeify<float>();
  RegionAccessor<AccessorType::Generic, Color> fa_node_color = 
    nodes.get_field_accessor(FID_NODE_COLOR).typeify<Color>();
  ptr_t *first_nodes = new ptr_t[num_pieces];
  {
    IndexAllocator node_allocator = runtime->create_index_allocator(ctx, ckt.all_nodes.get_index_space());
    node_allocator.alloc(num_pieces * nodes_per_piece);
  }
  {
    IndexIterator itr(runtime, ctx, ckt.all_nodes.get_index_space());
    for (int n = 0; n < num_pieces; n++)
    {
      for (int i = 0; i < nodes_per_piece; i++)
      {
        assert(itr.has_next());
        ptr_t node_ptr = itr.next();
        if (i == 0)
          first_nodes[n] = node_ptr;
        float capacitance = drand48() + 1.f;
        fa_node_cap.write(node_ptr, capacitance);
        float leakage = 0.1f * drand48();
        fa_node_leakage.write(node_ptr, leakage);
        fa_node_charge.write(node_ptr, 0.f);
        float init_voltage = 2*drand48() - 1.f;
        fa_node_voltage.write(node_ptr, init_voltage);
        fa_node_color.write(node_ptr, n);
        piece_node_ptrs[n].push_back(node_ptr);
      }
    }
  }

  wires.wait_until_valid();
  RegionAccessor<AccessorType::Generic, float> fa_wire_currents[WIRE_SEGMENTS];
  for (int i = 0; i < WIRE_SEGMENTS; i++)
    fa_wire_currents[i] = wires.get_field_accessor(FID_CURRENT+i).typeify<float>();
  RegionAccessor<AccessorType::Generic, float> fa_wire_voltages[WIRE_SEGMENTS-1];
  for (int i = 0; i < (WIRE_SEGMENTS-1); i++)
    fa_wire_voltages[i] = wires.get_field_accessor(FID_WIRE_VOLTAGE+i).typeify<float>();
  RegionAccessor<AccessorType::Generic, ptr_t> fa_wire_in_ptr = 
    wires.get_field_accessor(FID_IN_PTR).typeify<ptr_t>();
  RegionAccessor<AccessorType::Generic, ptr_t> fa_wire_out_ptr = 
    wires.get_field_accessor(FID_OUT_PTR).typeify<ptr_t>();
  RegionAccessor<AccessorType::Generic, float> fa_wire_inductance = 
    wires.get_field_accessor(FID_INDUCTANCE).typeify<float>();
  RegionAccessor<AccessorType::Generic, float> fa_wire_resistance = 
    wires.get_field_accessor(FID_RESISTANCE).typeify<float>();
  RegionAccessor<AccessorType::Generic, float> fa_wire_cap = 
    wires.get_field_accessor(FID_WIRE_CAP).typeify<float>();
  ptr_t *first_wires = new ptr_t[num_pieces];
  // Allocate all the wires
  {
    IndexAllocator wire_allocator = runtime->create_index_allocator(ctx, ckt.all_wires.get_index_space());
    wire_allocator.alloc(num_pieces * wires_per_piece);
  }
  {
    IndexIterator itr(runtime, ctx, ckt.all_wires.get_index_space());
    for (int n = 0; n < num_pieces; n++)
    {
      for (int i = 0; i < wires_per_piece; i++)
      {
        assert(itr.has_next());
        ptr_t wire_ptr = itr.next();
        // Record the first wire pointer for this piece
        if (i == 0)
          first_wires[n] = wire_ptr;
        for (int j = 0; j < WIRE_SEGMENTS; j++)
          fa_wire_currents[j].write(wire_ptr, 0.f);
        for (int j = 0; j < WIRE_SEGMENTS-1; j++) 
          fa_wire_voltages[j].write(wire_ptr, 0.f);

        float resistance = drand48() * 10.0 + 1.0;
        fa_wire_resistance.write(wire_ptr, resistance);
        // Keep inductance on the order of 1e-3 * dt to avoid resonance problems
        float inductance = (drand48() + 0.1) * DELTAT * 1e-3;
        fa_wire_inductance.write(wire_ptr, inductance);
        float capacitance = drand48() * 0.1;
        fa_wire_cap.write(wire_ptr, capacitance);

        fa_wire_in_ptr.write(wire_ptr, random_element(piece_node_ptrs[n])); 

        if ((100 * drand48()) < pct_wire_in_piece)
        {
          fa_wire_out_ptr.write(wire_ptr, random_element(piece_node_ptrs[n])); 
        }
        else
        {
          int nn = lrand48() % num_pieces;
          // Make sure we get a node from a different piece
          if (nn == n)
            nn = (nn + 1) % num_pieces;
          int idx = int(lrand48() % piece_node_ptrs[nn].size());
          ptr_t out_ptr = piece_node_ptrs[nn][idx];
          fa_wire_out_ptr.write(wire_ptr, out_ptr);
        }
      }
    }
  }

  runtime->unmap_region(ctx, wires);
  runtime->unmap_region(ctx, nodes);

  Partitions result;
  // First we compute a partitioning of the nodes based on the colors
  Domain color_space = Domain::from_rect<1>(Rect<1>(0, num_pieces-1));
  IndexPartition ip_nodes = 
    runtime->create_partition_by_field(ctx, ckt.all_nodes, ckt.all_nodes, 
                                       FID_NODE_COLOR, color_space); 
  runtime->attach_name(ip_nodes, "NODES PARTITIONED BY COLOR");
  LogicalPartition lp_locator = 
    runtime->get_logical_partition(ctx, ckt.node_locator, ip_nodes);
  runtime->attach_name(lp_locator, "LOCATOR PARTITION");
  result.node_locations = lp_locator;
  // Now partition the wires by mapping the node partition onto the wires
  IndexPartition ip_wires = 
    runtime->create_partition_by_preimage(ctx, ip_nodes, ckt.all_wires, ckt.all_wires,
                                          FID_IN_PTR, color_space, DISJOINT_KIND); 
  runtime->attach_name(ip_wires, "WIRES INDEX PARTITION");
  LogicalPartition lp_wires = 
    runtime->get_logical_partition(ctx, ckt.all_wires, ip_wires);
  runtime->attach_name(lp_wires, "WIRES LOGICAL PARTITION");
  result.pvt_wires = lp_wires; 

  IndexPartition ip_temp_one_hop = 
    runtime->create_partition_by_image(ctx, ckt.all_nodes.get_index_space(),
                                       lp_wires, ckt.all_wires, FID_OUT_PTR, color_space);
  runtime->attach_name(ip_temp_one_hop, "ALL NODES THAT OUT PTR POINTS TO");
  // Now compute the ghost partition
  IndexPartition ip_temp_ghost = 
    runtime->create_partition_by_difference(ctx, ckt.all_nodes.get_index_space(),
                                            ip_temp_one_hop, ip_nodes);
  runtime->attach_name(ip_temp_ghost, "PARTITION FOR GHOST NODES");

  // Create the pending partition for all private and all shared regions
  Domain all_color_space = Domain::from_rect<1>(Rect<1>(0, 1));
  IndexPartition ip_all = 
    runtime->create_pending_partition(ctx, ckt.all_nodes.get_index_space(),
                                      all_color_space, DISJOINT_KIND);
  runtime->attach_name(ip_all, "ALL INDEX PARTITION");
  LogicalPartition lp_all = 
    runtime->get_logical_partition(ctx, ckt.all_nodes, ip_all);
  runtime->attach_name(lp_all, "ALL LOGICAL PARTITION");
  // Compute each of the subregions
  DomainPoint all_private_color = DomainPoint::from_point<1>(Point<1>(0));
  DomainPoint all_shared_color = DomainPoint::from_point<1>(Point<1>(1));

  IndexSpace is_all_shared = 
    runtime->create_index_space_union(ctx, ip_all, all_shared_color, ip_temp_ghost);
  runtime->attach_name(is_all_shared, "ALL SHARED INDEX SPACE");

  std::vector<IndexSpace> diff_spaces;
  diff_spaces.push_back(is_all_shared);
  IndexSpace is_all_private = 
    runtime->create_index_space_difference(ctx, ip_all, all_private_color,
                                           ckt.all_nodes.get_index_space(),
                                           diff_spaces);
  runtime->attach_name(is_all_private, "ALL PRIVATE INDEX SPACE");
  LogicalRegion lr_all_private = 
    runtime->get_logical_subregion(ctx, lp_all, is_all_private);
  runtime->attach_name(lr_all_private, "ALL PRIVATE NODES");
  LogicalRegion lr_all_shared = 
    runtime->get_logical_subregion(ctx, lp_all, is_all_shared);
  runtime->attach_name(lr_all_shared, "ALL SHARED NODES");
  // Now compute the sub-partitions
  IndexPartition ip_private = 
    runtime->create_partition_by_field(ctx, lr_all_private, ckt.all_nodes,
                                       FID_NODE_COLOR, color_space);
  runtime->attach_name(ip_private, "PRIVATE INDEX SPACE PARTITION");
  result.pvt_nodes = runtime->get_logical_partition(ctx, lr_all_private, ip_private);
  IndexPartition ip_shared = 
    runtime->create_partition_by_field(ctx, lr_all_shared, ckt.all_nodes,
                                       FID_NODE_COLOR, color_space);
  runtime->attach_name(ip_shared, "SHARED INDEX SPACE PARTITION");
  result.shr_nodes = runtime->get_logical_partition(ctx, lr_all_shared, ip_shared);
  // Ghost sub-partition 
  IndexPartition ip_shared_one_hop = 
    runtime->create_partition_by_image(ctx, is_all_shared, lp_wires, 
                                       ckt.all_wires, FID_OUT_PTR, color_space);
  runtime->attach_name(ip_shared_one_hop, "SHARED NODES THAT OUT PTR POINTS TO");
  // Now compute the ghost partition
  IndexPartition ip_ghost = 
    runtime->create_partition_by_difference(ctx, is_all_shared,
                                            ip_shared_one_hop, ip_shared);
  runtime->attach_name(ip_ghost, "GHOST INDEX SPACE PARTITION");
  result.ghost_nodes = runtime->get_logical_partition(ctx, lr_all_shared, ip_ghost);

  char buf[100];
  // Build the pieces
  for (int n = 0; n < num_pieces; n++)
  {
    pieces[n].pvt_nodes = 
      runtime->get_logical_subregion_by_color(ctx, result.pvt_nodes, n);
    sprintf(buf, "PRIVATE NODES OF PIECE %d", n);
    runtime->attach_name(pieces[n].pvt_nodes, buf);
    pieces[n].shr_nodes = 
      runtime->get_logical_subregion_by_color(ctx, result.shr_nodes, n);
    sprintf(buf, "SHARED NODES OF PIECE %d", n);
    runtime->attach_name(pieces[n].shr_nodes, buf);
    pieces[n].ghost_nodes = 
      runtime->get_logical_subregion_by_color(ctx, result.ghost_nodes, n);
    sprintf(buf, "GHOST NODES OF PIECE %d", n);
    runtime->attach_name(pieces[n].ghost_nodes, buf);
    pieces[n].pvt_wires = 
      runtime->get_logical_subregion_by_color(ctx, result.pvt_wires, n);
    sprintf(buf, "PRIVATE WIRES OF PIECE %d", n);
    runtime->attach_name(pieces[n].pvt_wires, buf);
    pieces[n].num_wires = wires_per_piece;
    pieces[n].first_wire = first_wires[n];
    pieces[n].num_nodes = nodes_per_piece;
    pieces[n].first_node = first_nodes[n];

    pieces[n].dt = DELTAT;
    pieces[n].steps = steps;
    // Assign the locations
    LogicalRegion lr_locator = 
      runtime->get_logical_subregion_by_color(ctx, result.node_locations, n); 
    RegionRequirement locator_req(lr_locator, READ_WRITE, EXCLUSIVE, ckt.node_locator);
    locator_req.add_field(FID_LOCATOR);
    PhysicalRegion locator = runtime->map_region(ctx, locator_req);
    RegionRequirement wire_loc_req(pieces[n].pvt_wires, READ_WRITE, 
                                   EXCLUSIVE, ckt.all_wires);
    wire_loc_req.add_field(FID_IN_PTR);
    wire_loc_req.add_field(FID_OUT_PTR);
    wire_loc_req.add_field(FID_IN_LOC);
    wire_loc_req.add_field(FID_OUT_LOC);
    PhysicalRegion wire_loc = runtime->map_region(ctx, wire_loc_req);
    locator.wait_until_valid();
    RegionAccessor<AccessorType::Generic, PointerLocation> locator_acc = 
      locator.get_field_accessor(FID_LOCATOR).typeify<PointerLocation>();
    {
      IndexIterator itr(runtime, ctx, lr_locator.get_index_space());
      while (itr.has_next()) {
        ptr_t node_ptr = itr.next();
        ptr_t pvt_ptr = runtime->safe_cast(ctx, node_ptr, pieces[n].pvt_nodes);
        ptr_t shr_ptr = runtime->safe_cast(ctx, node_ptr, pieces[n].shr_nodes);
        // Make sure only one of these is non-null
        assert((pvt_ptr.is_null() && !shr_ptr.is_null()) || 
               (shr_ptr.is_null() && !pvt_ptr.is_null()));
        if (!pvt_ptr.is_null())
          locator_acc.write(node_ptr, PRIVATE_PTR);
        else
          locator_acc.write(node_ptr, SHARED_PTR);
      }
    }
    runtime->unmap_region(ctx, locator);
    wire_loc.wait_until_valid();
    RegionAccessor<AccessorType::Generic, ptr_t> fa_wire_in_ptr = 
      wire_loc.get_field_accessor(FID_IN_PTR).typeify<ptr_t>();
    RegionAccessor<AccessorType::Generic, ptr_t> fa_wire_out_ptr = 
      wire_loc.get_field_accessor(FID_OUT_PTR).typeify<ptr_t>();
    RegionAccessor<AccessorType::Generic, PointerLocation> fa_wire_in_loc = 
      wire_loc.get_field_accessor(FID_IN_LOC).typeify<PointerLocation>();
    RegionAccessor<AccessorType::Generic, PointerLocation> fa_wire_out_loc = 
      wire_loc.get_field_accessor(FID_OUT_LOC).typeify<PointerLocation>();
    {
      IndexIterator itr(runtime, ctx, pieces[n].pvt_wires.get_index_space());
      while (itr.has_next()) {
        ptr_t wire_ptr = itr.next();
        ptr_t in_ptr = fa_wire_in_ptr.read(wire_ptr);
        ptr_t out_ptr = fa_wire_out_ptr.read(wire_ptr);
        fa_wire_in_loc.write(wire_ptr, 
            find_location(in_ptr, runtime, ctx, 
                          pieces[n].pvt_nodes,
                          pieces[n].shr_nodes,
                          pieces[n].ghost_nodes));
        fa_wire_out_loc.write(wire_ptr,
            find_location(out_ptr, runtime, ctx,
                          pieces[n].pvt_nodes,
                          pieces[n].shr_nodes,
                          pieces[n].ghost_nodes));
      }
    }
    runtime->unmap_region(ctx, wire_loc);
  }

  delete [] first_wires;
  delete [] first_nodes;

  log_circuit.print("Finished initializing simulation...");

  return result;
}

