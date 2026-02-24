// src/main.rs
mod dataflow;
mod cost_model;
mod extractor;
mod parser;

use clap::Parser as ClapParser;
use std::fs;
use std::path::PathBuf;

use parser::BlockParser;
use cost_model::{CostModel, ObjectiveWeights, Cost};
use extractor::{BlockExtractor, EGraphData, BlockImpl};

#[derive(ClapParser)]
#[command(name = "mapper")]
#[command(about = "Multi-objective optimization for heterogeneous systems")]
#[command(version = "0.1.0")]
struct Cli {
    /// Input .egg file
    input: PathBuf,

    /// Optimization mode
    #[arg(short, long, default_value = "balanced")]
    mode: OptMode,

    /// Enable verbose output
    #[arg(short, long)]
    verbose: bool,

    /// Show Pareto front (multi-objective)
    #[arg(long)]
    pareto: bool,

    /// Show best for each objective
    #[arg(long)]
    per_objective: bool,
}

#[derive(Clone, Debug)]
enum OptMode {
    Balanced,
    MinLatency,
    MaxThroughput,
    MinEnergy,
}

impl std::str::FromStr for OptMode {
    type Err = String;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "balanced" => Ok(OptMode::Balanced),
            "min-latency" => Ok(OptMode::MinLatency),
            "max-throughput" => Ok(OptMode::MaxThroughput),
            "min-energy" => Ok(OptMode::MinEnergy),
            _ => Err(format!("Unknown mode: {}. Valid modes: balanced, min-latency, max-throughput, min-energy", s)),
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();

    // Step 1: Parse input file
    if cli.verbose {
        println!("=== Step 1: Parsing {} ===", cli.input.display());
    }

    let input = fs::read_to_string(&cli.input)?;
    let mut parser = BlockParser::new();
    let graph = parser.parse_file(Some(cli.input.to_string_lossy().to_string()), &input)?;

    if cli.verbose {
        graph.print_summary();
    }

    // Step 2: Build e-graph data
    if cli.verbose {
        println!("\n=== Step 2: Building e-graph data ===");
    }

    let egraph_data = build_egraph_data(&graph);

    if cli.verbose {
        println!("  Total blocks: {}", egraph_data.block_equivalences.len());
        println!("  Total implementations: {}", 
            egraph_data.block_equivalences.values().map(|v| v.len()).sum::<usize>());
    }

    // Step 3: Set up cost model
    if cli.verbose {
        println!("\n=== Step 3: Setting up cost model ===");
        println!("  Optimization mode: {:?}", cli.mode);
    }

    let weights = match cli.mode {
        OptMode::Balanced => ObjectiveWeights::balanced(),
        OptMode::MinLatency => ObjectiveWeights::min_latency(),
        OptMode::MaxThroughput => ObjectiveWeights::max_throughput(),
        OptMode::MinEnergy => ObjectiveWeights::min_energy(),
    };

    let cost_model = setup_cost_model(&parser, &graph, weights);

    if cli.verbose {
        println!("  Loaded {} operation costs", cost_model.op_costs.len());
        println!("  Hardware topology: {} types, {} links",
            cost_model.topology.hardware_types.len(),
            cost_model.topology.links.len());
        
        println!("  Communication links:");
        for ((from, to), link) in &cost_model.topology.links {
            println!("    {} -> {}: latency={:.2}, bandwidth={:.2}",
                from, to, link.latency_per_unit, link.bandwidth_factor);
        }
    }

    // Step 4: Extract optimal mapping(s)
    if cli.verbose {
        println!("\n=== Step 4: Extracting optimal mapping ===");
    }

    let extractor = BlockExtractor::new(&egraph_data, &cost_model);

    if cli.pareto {
        // Multi-objective: show Pareto front
        if cli.verbose {
            println!("  Computing Pareto front...");
        }
        let pareto = extractor.extract_pareto_front();
        print_pareto_front(&pareto, cli.verbose);
    } else if cli.per_objective {
        // Show best for each objective
        if cli.verbose {
            println!("  Computing best solution for each objective...");
        }
        let solutions = extractor.extract_per_objective();
        print_per_objective(&solutions, cli.verbose);
    } else {
        // Single-objective: show best weighted solution
        if cli.verbose {
            println!("  Computing best weighted solution...");
        }
        if let Some(mapping) = extractor.extract_best_mapping() {
            print_results(&mapping, &cost_model, cli.verbose);
        } else {
            println!("\n❌ No feasible mapping found!");
            println!("This may be due to:");
            println!("  - Disconnected hardware preventing required data transfers");
            println!("  - No valid implementations for some blocks");
        }
    }

    Ok(())
}

/// Build e-graph data from parsed dataflow graph
fn build_egraph_data(graph: &dataflow::DataflowGraph) -> EGraphData {
    let mut egraph = EGraphData::new();

    for (block_id, block) in &graph.blocks {
        let mut implementations = Vec::new();

        for impl_def in &block.implementations {
            let ops = extract_op_names(&impl_def.ops);

            implementations.push(BlockImpl {
                name: impl_def.name.clone(),
                hw: impl_def.hardware.clone(),
                ops,
            });
        }

        egraph
            .block_equivalences
            .insert(block_id.clone(), implementations);

        // Extract dependencies from first implementation (should be same for all)
        if let Some(first_impl) = block.implementations.first() {
            egraph
                .dependencies
                .insert(block_id.clone(), first_impl.deps.clone());
        }
    }

    egraph
}

/// Extract operation names from block operations
fn extract_op_names(ops: &[dataflow::Operation]) -> Vec<String> {
    ops.iter()
        .map(|op| match &op.expr {
            dataflow::OpExpr::Call { func, .. } => func.clone(),
            dataflow::OpExpr::Var(name) => name.clone(),
            dataflow::OpExpr::Literal(_) => "literal".to_string(),
        })
        .collect()
}

/// Set up cost model from parsed data
fn setup_cost_model(
    parser: &BlockParser,
    graph: &dataflow::DataflowGraph,
    weights: ObjectiveWeights,
) -> CostModel {
    let mut cost_model = CostModel::from_topology(graph.topology.clone(), weights);

    // Load operation costs from parser
    // Parser provides latency from :cost annotation
    for (op_name, latency) in &parser.op_costs {
        for hw_name in &graph.topology.hardware_types {
            let hw = dataflow::Hardware::new(hw_name.clone());
            
            // Model hardware characteristics based on common types
            // You can customize these multipliers based on hardware type
            let (latency_factor, throughput, energy_factor) = match hw_name.as_str() {
                "CPU" | "ARM_CPU" | "x86_CPU" => (1.5, 2.0, 2.0),   // Slower, low throughput, high energy
                "DRMT" | "SmartNIC" => (1.0, 5.0, 1.0),              // Baseline
                "DPA" | "FPGA" => (0.5, 10.0, 0.5),                  // Faster, high throughput, low energy
                _ => (1.0, 1.0, 1.0), // Default for unknown hardware
            };

            let op_latency = latency * latency_factor;
            let op_energy = latency * energy_factor;

            cost_model.set_op_cost(op_name, &hw, Cost::new(op_latency, throughput, op_energy));
        }
    }

    // Set default costs for operations not explicitly defined in parser
    for block in graph.blocks.values() {
        for impl_def in &block.implementations {
            for op in &impl_def.ops {
                let op_name = match &op.expr {
                    dataflow::OpExpr::Call { func, .. } => func.clone(),
                    dataflow::OpExpr::Var(name) => name.clone(),
                    _ => continue,
                };

                // Check if already set
                let current_cost = cost_model.get_op_cost(&op_name, &impl_def.hardware);
                if current_cost.latency == 1.0 && current_cost.energy == 1.0 {
                    // Not set, use hardware-specific defaults
                    let hw_name = impl_def.hardware.name();
                    let default_cost = match hw_name {
                        "CPU" | "ARM_CPU" | "x86_CPU" => Cost::new(10.0, 2.0, 20.0),
                        "DRMT" | "SmartNIC" => Cost::new(5.0, 5.0, 10.0),
                        "DPA" | "FPGA" => Cost::new(2.0, 10.0, 3.0),
                        _ => Cost::new(5.0, 5.0, 10.0),
                    };
                    cost_model.set_op_cost(&op_name, &impl_def.hardware, default_cost);
                }
            }
        }
    }

    // Set data transfer sizes (default to 1.0 unit)
    for block_id in graph.blocks.keys() {
        cost_model.transfer_sizes.insert(block_id.clone(), 1.0);
    }

    cost_model
}

/// Print single-objective optimization result
fn print_results(mapping: &cost_model::BlockMapping, cost_model: &CostModel, verbose: bool) {
    println!("\n╔═══════════════════════════════════════════════════════════════╗");
    println!("║                    OPTIMAL MAPPING                            ║");
    println!("╚═══════════════════════════════════════════════════════════════╝\n");

    println!("Feasibility: {}", if mapping.is_feasible { "✓ FEASIBLE" } else { "✗ INFEASIBLE" });
    println!();

    println!("Multi-objective costs:");
    println!("  Latency:     {:.2} cycles", mapping.total_cost.total_latency());
    println!("  Throughput:  {:.2} ops/cycle", mapping.total_cost.throughput());
    println!("  Energy:      {:.2} J", mapping.total_cost.energy);
    println!();

    println!("Weighted cost: {:.2}", mapping.total_cost.weighted_sum(&cost_model.weights));
    println!("  (weights: latency={:.2}, throughput={:.2}, energy={:.2})",
        cost_model.weights.latency,
        cost_model.weights.throughput,
        cost_model.weights.energy);
    println!();

    println!("Block assignments:");
    println!("┌────────────────────┬──────────┬─────────────────────────────────────┐");
    println!("│ Block Name         │ Hardware │ Cost                                │");
    println!("├────────────────────┼──────────┼─────────────────────────────────────┤");
    
    for block in &mapping.blocks {
        if verbose {
            println!("│ {:18} │ {:8} │ {}                    │", 
                block.name, 
                block.hw.name(),
                block.cost);
            println!("│                    │          │   Ops: {:?}     │", block.ops);
            if !block.deps.is_empty() {
                println!("│                    │          │   Deps: {:?}   │", block.deps);
            }
            println!("├────────────────────┼──────────┼─────────────────────────────────────┤");
        } else {
            println!("│ {:18} │ {:8} │ lat:{:.1} tput:{:.1} energy:{:.1}     │", 
                block.name, 
                block.hw.name(),
                block.cost.latency,
                block.cost.throughput(),
                block.cost.energy);
        }
    }
    println!("└────────────────────┴──────────┴─────────────────────────────────────┘");

    if verbose {
        println!("\nCost breakdown:");
        let compute_latency: f64 = mapping.blocks.iter().map(|b| b.cost.latency).sum();
        let comm_latency = mapping.total_cost.communication;
        let total_energy = mapping.total_cost.energy;
        
        println!("  Computation latency: {:.2} cycles", compute_latency);
        println!("  Communication latency: {:.2} cycles", comm_latency);
        println!("  Total latency: {:.2} cycles", compute_latency + comm_latency);
        println!("  Total energy: {:.2} J", total_energy);
    }
}

/// Print Pareto front
fn print_pareto_front(pareto: &[cost_model::BlockMapping], verbose: bool) {
    println!("\n╔═══════════════════════════════════════════════════════════════╗");
    println!("║                    PARETO FRONT                               ║");
    println!("╚═══════════════════════════════════════════════════════════════╝\n");

    println!("Found {} Pareto-optimal solutions:\n", pareto.len());

    if pareto.is_empty() {
        println!("❌ No feasible solutions found!");
        return;
    }

    for (i, mapping) in pareto.iter().enumerate() {
        println!("┌─ Solution {} ───────────────────────────────────────────────────┐", i + 1);
        println!("│ Latency:    {:.2} cycles                                      │", mapping.total_cost.total_latency());
        println!("│ Throughput: {:.2} ops/cycle                                   │", mapping.total_cost.throughput());
        println!("│ Energy:     {:.2} J                                           │", mapping.total_cost.energy);
        println!("│                                                                │");
        
        for block in &mapping.blocks {
            if verbose {
                println!("│   {:18} -> {:8}                              │", 
                    block.name, block.hw.name());
                println!("│     {}                                │", block.cost);
            } else {
                println!("│   {:18} -> {:8}                              │", 
                    block.name, block.hw.name());
            }
        }
        println!("└────────────────────────────────────────────────────────────────┘");
        println!();
    }

    // Print comparison table
    println!("\nComparison table:");
    println!("┌──────────┬────────────┬──────────────┬────────────┐");
    println!("│ Solution │ Latency    │ Throughput   │ Energy     │");
    println!("├──────────┼────────────┼──────────────┼────────────┤");
    for (i, mapping) in pareto.iter().enumerate() {
        println!("│ {:8} │ {:10.2} │ {:12.2} │ {:10.2} │",
            i + 1,
            mapping.total_cost.total_latency(),
            mapping.total_cost.throughput(),
            mapping.total_cost.energy);
    }
    println!("└──────────┴────────────┴──────────────┴────────────┘");
}

/// Print best solution for each individual objective
fn print_per_objective(solutions: &extractor::ObjectiveSolutions, verbose: bool) {
    println!("\n╔═══════════════════════════════════════════════════════════════╗");
    println!("║            OPTIMAL SOLUTIONS PER OBJECTIVE                    ║");
    println!("╚═══════════════════════════════════════════════════════════════╝\n");

    // Minimum Latency
    if let Some(mapping) = &solutions.min_latency {
        println!("┌─ MINIMUM LATENCY ──────────────────────────────────────────┐");
        println!("│ Total latency: {:.2} cycles                                 │", mapping.total_cost.total_latency());
        println!("│ (Throughput: {:.2} ops/cycle, Energy: {:.2} J)             │", 
            mapping.total_cost.throughput(), mapping.total_cost.energy);
        println!("│                                                              │");
        print_mapping_summary(mapping, verbose);
        println!("└──────────────────────────────────────────────────────────────┘");
        println!();
    } else {
        println!("❌ No feasible solution for minimum latency");
        println!();
    }

    // Maximum Throughput
    if let Some(mapping) = &solutions.max_throughput {
        println!("┌─ MAXIMUM THROUGHPUT ────────────────────────────────────────┐");
        println!("│ Throughput: {:.2} ops/cycle                                 │", mapping.total_cost.throughput());
        println!("│ (Latency: {:.2} cycles, Energy: {:.2} J)                    │", 
            mapping.total_cost.total_latency(), mapping.total_cost.energy);
        println!("│                                                              │");
        print_mapping_summary(mapping, verbose);
        println!("└──────────────────────────────────────────────────────────────┘");
        println!();
    } else {
        println!("❌ No feasible solution for maximum throughput");
        println!();
    }

    // Minimum Energy
    if let Some(mapping) = &solutions.min_energy {
        println!("┌─ MINIMUM ENERGY ────────────────────────────────────────────┐");
        println!("│ Total energy: {:.2} J                                       │", mapping.total_cost.energy);
        println!("│ (Latency: {:.2} cycles, Throughput: {:.2} ops/cycle)       │", 
            mapping.total_cost.total_latency(), mapping.total_cost.throughput());
        println!("│                                                              │");
        print_mapping_summary(mapping, verbose);
        println!("└──────────────────────────────────────────────────────────────┘");
        println!();
    } else {
        println!("❌ No feasible solution for minimum energy");
        println!();
    }
}

/// Print summary of a mapping
fn print_mapping_summary(mapping: &cost_model::BlockMapping, verbose: bool) {
    for block in &mapping.blocks {
        if verbose {
            println!("│   {:18} -> {:8}                            │", 
                block.name, block.hw.name());
            println!("│     lat:{:.1} tput:{:.1} energy:{:.1}                         │",
                block.cost.latency,
                block.cost.throughput(),
                block.cost.energy);
            println!("│     ops: {:?}                                    │", block.ops);
        } else {
            println!("│   {:18} -> {:8}                            │", 
                block.name, block.hw.name());
        }
    }
}