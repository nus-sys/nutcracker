// src/extractor.rs
use crate::cost_model::*;
use crate::dataflow::{Hardware, HardwareTopology};
use std::collections::HashMap;

pub struct EGraphData {
    pub block_equivalences: HashMap<String, Vec<BlockImpl>>,
    pub dependencies: HashMap<String, Vec<String>>,
}

impl EGraphData {
    pub fn new() -> Self {
        Self {
            block_equivalences: HashMap::new(),
            dependencies: HashMap::new(),
        }
    }
}

impl Default for EGraphData {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Clone)]
pub struct BlockImpl {
    pub name: String,
    pub hw: Hardware,
    pub ops: Vec<String>,
}

pub struct BlockExtractor<'a> {
    egraph: &'a EGraphData,
    cost_model: &'a CostModel,
}

impl<'a> BlockExtractor<'a> {
    pub fn new(egraph: &'a EGraphData, cost_model: &'a CostModel) -> Self {
        Self { egraph, cost_model }
    }

    /// Extract best feasible mapping (single-objective)
    pub fn extract_best_mapping(&self) -> Option<BlockMapping> {
        let mut block_names: Vec<_> = self.egraph.block_equivalences.keys().cloned().collect();
        
        // Sort for deterministic ordering and better cache locality
        block_names.sort();

        if block_names.is_empty() {
            return Some(BlockMapping::new());
        }

        let mut best_mapping = None;
        let mut best_cost = f64::INFINITY;

        // Use a map to track which implementation was chosen for each block
        let mut chosen_impls: HashMap<String, usize> = HashMap::new();
        // Pre-allocate space
        chosen_impls.reserve(block_names.len());

        self.search_assignments(
            &block_names,
            0,
            &mut chosen_impls,
            &mut best_mapping,
            &mut best_cost,
        );

        best_mapping
    }

    fn search_assignments(
        &self,
        block_names: &[String],
        idx: usize,
        chosen_impls: &mut HashMap<String, usize>,
        best_mapping: &mut Option<BlockMapping>,
        best_cost: &mut f64,
    ) {
        if idx == block_names.len() {
            // Build the mapping from chosen implementations
            let mut blocks = Vec::with_capacity(block_names.len());
            for block_name in block_names {
                let impl_idx = chosen_impls[block_name];
                let impl_def = &self.egraph.block_equivalences[block_name][impl_idx];
                
                // Avoid cloning dependencies by using get without cloned
                let deps = self
                    .egraph
                    .dependencies
                    .get(block_name)
                    .map(|d| d.as_slice())
                    .unwrap_or(&[]);

                let costed = CostedBlock::new(
                    impl_def.name.clone(),
                    impl_def.hw.clone(),
                    impl_def.ops.clone(),
                    deps.to_vec(),
                    self.cost_model,
                );
                blocks.push(costed);
            }

            let mut mapping = BlockMapping {
                blocks,
                total_cost: Cost::zero(),
                is_feasible: true,
            };
            mapping.compute_total_cost(self.cost_model);

            if !mapping.is_feasible {
                return;
            }

            let weighted_cost = mapping.weighted_cost(self.cost_model);
            if weighted_cost < *best_cost {
                *best_cost = weighted_cost;
                *best_mapping = Some(mapping);
            }
            return;
        }

        // Early pruning: if current cost already exceeds best, skip
        // (This would require incremental cost computation for maximum effect)

        let block_name = &block_names[idx];
        let implementations = &self.egraph.block_equivalences[block_name];

        // Get dependencies once
        let deps = self
            .egraph
            .dependencies
            .get(block_name)
            .map(|d| d.as_slice())
            .unwrap_or(&[]);

        for (impl_idx, impl_def) in implementations.iter().enumerate() {
            // Check if this implementation is feasible given already chosen implementations
            let mut feasible = true;
            for dep_block_name in deps {
                // Check if dependency has been assigned
                if let Some(&dep_impl_idx) = chosen_impls.get(dep_block_name) {
                    let dep_impl = &self.egraph.block_equivalences[dep_block_name][dep_impl_idx];
                    if !self.cost_model.is_connected(&dep_impl.hw, &impl_def.hw) {
                        feasible = false;
                        break;
                    }
                }
            }

            if !feasible {
                continue;
            }

            // Choose this implementation
            chosen_impls.insert(block_name.clone(), impl_idx);
            self.search_assignments(block_names, idx + 1, chosen_impls, best_mapping, best_cost);
            chosen_impls.remove(block_name);
        }
    }

    /// Extract Pareto-optimal solutions (multi-objective)
    pub fn extract_pareto_front(&self) -> Vec<BlockMapping> {
        let mut block_names: Vec<_> = self.egraph.block_equivalences.keys().cloned().collect();
        
        // Sort for deterministic ordering
        block_names.sort();

        if block_names.is_empty() {
            return vec![BlockMapping::new()];
        }

        let mut all_feasible = Vec::new();
        let mut chosen_impls: HashMap<String, usize> = HashMap::new();
        chosen_impls.reserve(block_names.len());
        
        self.collect_all_feasible(&block_names, 0, &mut chosen_impls, &mut all_feasible);

        // Filter to Pareto front
        self.compute_pareto_front(all_feasible)
    }

    fn collect_all_feasible(
        &self,
        block_names: &[String],
        idx: usize,
        chosen_impls: &mut HashMap<String, usize>,
        all_feasible: &mut Vec<BlockMapping>,
    ) {
        if idx == block_names.len() {
            // Build the mapping from chosen implementations
            let mut blocks = Vec::with_capacity(block_names.len());
            for block_name in block_names {
                let impl_idx = chosen_impls[block_name];
                let impl_def = &self.egraph.block_equivalences[block_name][impl_idx];
                
                let deps = self
                    .egraph
                    .dependencies
                    .get(block_name)
                    .map(|d| d.as_slice())
                    .unwrap_or(&[]);

                let costed = CostedBlock::new(
                    impl_def.name.clone(),
                    impl_def.hw.clone(),
                    impl_def.ops.clone(),
                    deps.to_vec(),
                    self.cost_model,
                );
                blocks.push(costed);
            }

            let mut mapping = BlockMapping {
                blocks,
                total_cost: Cost::zero(),
                is_feasible: true,
            };
            mapping.compute_total_cost(self.cost_model);

            if mapping.is_feasible {
                all_feasible.push(mapping);
            }
            return;
        }

        let block_name = &block_names[idx];
        let implementations = &self.egraph.block_equivalences[block_name];
        
        // Get dependencies once
        let deps = self
            .egraph
            .dependencies
            .get(block_name)
            .map(|d| d.as_slice())
            .unwrap_or(&[]);

        for (impl_idx, impl_def) in implementations.iter().enumerate() {
            let mut feasible = true;
            for dep_block_name in deps {
                if let Some(&dep_impl_idx) = chosen_impls.get(dep_block_name) {
                    let dep_impl = &self.egraph.block_equivalences[dep_block_name][dep_impl_idx];
                    if !self.cost_model.is_connected(&dep_impl.hw, &impl_def.hw) {
                        feasible = false;
                        break;
                    }
                }
            }

            if !feasible {
                continue;
            }

            chosen_impls.insert(block_name.clone(), impl_idx);
            self.collect_all_feasible(block_names, idx + 1, chosen_impls, all_feasible);
            chosen_impls.remove(block_name);
        }
    }

    /// Compute Pareto front from all feasible solutions
    fn compute_pareto_front(&self, solutions: Vec<BlockMapping>) -> Vec<BlockMapping> {
        let mut pareto_front = Vec::new();

        for candidate in &solutions {
            let mut dominated = false;

            for other in &solutions {
                if other.total_cost.dominates(&candidate.total_cost) {
                    dominated = true;
                    break;
                }
            }

            if !dominated {
                pareto_front.push(candidate.clone());
            }
        }

        pareto_front
    }

    /// Extract optimal for each single objective
    pub fn extract_per_objective(&self) -> ObjectiveSolutions {
        let mut block_names: Vec<_> = self.egraph.block_equivalences.keys().cloned().collect();
        
        // Sort for deterministic ordering
        block_names.sort();

        if block_names.is_empty() {
            return ObjectiveSolutions {
                min_latency: Some(BlockMapping::new()),
                max_throughput: Some(BlockMapping::new()),
                min_energy: Some(BlockMapping::new()),
            };
        }

        let mut all_feasible = Vec::new();
        let mut chosen_impls: HashMap<String, usize> = HashMap::new();
        chosen_impls.reserve(block_names.len());
        
        self.collect_all_feasible(&block_names, 0, &mut chosen_impls, &mut all_feasible);

        ObjectiveSolutions {
            min_latency: self.find_best_by(|m| m.total_cost.total_latency(), &all_feasible),
            max_throughput: self.find_best_by(|m| m.total_cost.inverse_throughput, &all_feasible),
            min_energy: self.find_best_by(|m| m.total_cost.energy, &all_feasible),
        }
    }

    fn find_best_by<F>(&self, metric: F, solutions: &[BlockMapping]) -> Option<BlockMapping>
    where
        F: Fn(&BlockMapping) -> f64,
    {
        solutions
            .iter()
            .min_by(|a, b| metric(a).partial_cmp(&metric(b)).unwrap())
            .cloned()
    }
}

/// Solutions optimized for each individual objective
pub struct ObjectiveSolutions {
    pub min_latency: Option<BlockMapping>,
    pub max_throughput: Option<BlockMapping>,
    pub min_energy: Option<BlockMapping>,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_topology() -> HardwareTopology {
        let mut topo = HardwareTopology::new();
        topo.add_hardware("CPU".to_string());
        topo.add_hardware("DRMT".to_string());
        topo.add_hardware("DPA".to_string());

        // Connected paths
        topo.add_link("CPU".to_string(), "DRMT".to_string(), 10.0, 1.0);
        topo.add_link("DRMT".to_string(), "CPU".to_string(), 10.0, 1.0);
        topo.add_link("CPU".to_string(), "DPA".to_string(), 50.0, 0.5);
        topo.add_link("DPA".to_string(), "CPU".to_string(), 50.0, 0.5);

        // DRMT and DPA are disconnected

        topo
    }

    #[test]
    fn test_simple_extraction() {
        let mut egraph = EGraphData::new();

        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );
        egraph.dependencies.insert("blk1".to_string(), vec![]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        
        cost_model.set_op_cost("op1", &cpu, Cost::new(10.0, 2.0, 20.0));
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 10.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping();

        assert!(result.is_some());
        let mapping = result.unwrap();
        assert!(mapping.is_feasible);
        assert_eq!(mapping.blocks.len(), 1);
    }

    #[test]
    fn test_multi_objective_extraction() {
        let mut egraph = EGraphData::new();

        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );
        egraph.dependencies.insert("blk1".to_string(), vec![]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        // CPU: high latency, low throughput, high energy
        cost_model.set_op_cost("op1", &cpu, Cost::new(10.0, 2.0, 20.0));
        // DRMT: medium latency, medium throughput, medium energy
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 10.0));
        // DPA: low latency, high throughput, low energy
        cost_model.set_op_cost("op1", &dpa, Cost::new(2.0, 10.0, 3.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let solutions = extractor.extract_per_objective();

        // DPA should win all objectives
        assert_eq!(solutions.min_latency.unwrap().blocks[0].hw.name(), "DPA");
        assert_eq!(solutions.max_throughput.unwrap().blocks[0].hw.name(), "DPA");
        assert_eq!(solutions.min_energy.unwrap().blocks[0].hw.name(), "DPA");
    }

    #[test]
    fn test_pareto_front_multiple_solutions() {
        let mut egraph = EGraphData::new();

        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );
        egraph.dependencies.insert("blk1".to_string(), vec![]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        
        // CPU: fast but high energy
        cost_model.set_op_cost("op1", &cpu, Cost::new(2.0, 10.0, 20.0));
        // DRMT: slow but low energy
        cost_model.set_op_cost("op1", &drmt, Cost::new(10.0, 5.0, 5.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let pareto = extractor.extract_pareto_front();

        // Both should be on Pareto front (trade-off)
        assert_eq!(pareto.len(), 2);
    }

    #[test]
    fn test_multi_block_disconnected_hardware() {
        let mut egraph = EGraphData::new();

        // Block 1: only CPU implementation
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![BlockImpl {
                name: "blk1_cpu".to_string(),
                hw: Hardware::new("CPU".to_string()),
                ops: vec!["op1".to_string()],
            }],
        );

        // Block 2: depends on blk1, only DRMT implementation
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![BlockImpl {
                name: "blk2_drmt".to_string(),
                hw: Hardware::new("DRMT".to_string()),
                ops: vec!["op2".to_string()],
            }],
        );

        // Block 3: depends on blk2, only DPA implementation
        egraph.block_equivalences.insert(
            "blk3".to_string(),
            vec![BlockImpl {
                name: "blk3_dpa".to_string(),
                hw: Hardware::new("DPA".to_string()),
                ops: vec!["op3".to_string()],
            }],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);
        egraph.dependencies.insert("blk3".to_string(), vec!["blk2".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        cost_model.set_op_cost("op1", &cpu, Cost::new(10.0, 2.0, 20.0));
        cost_model.set_op_cost("op2", &drmt, Cost::new(5.0, 5.0, 10.0));
        cost_model.set_op_cost("op3", &dpa, Cost::new(2.0, 10.0, 5.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping();

        // Should be None because DRMT->DPA is disconnected
        assert!(result.is_none());
    }

    #[test]
    fn test_multi_block_with_alternatives() {
        let mut egraph = EGraphData::new();

        // Block 1: CPU and DRMT implementations
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        // Block 2: depends on blk1, CPU and DRMT implementations
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::min_latency());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        
        // DRMT is faster
        cost_model.set_op_cost("op1", &cpu, Cost::new(20.0, 2.0, 20.0));
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 10.0));
        cost_model.set_op_cost("op2", &cpu, Cost::new(15.0, 3.0, 15.0));
        cost_model.set_op_cost("op2", &drmt, Cost::new(3.0, 8.0, 8.0));

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        
        // Find blocks by name
        let blk1 = result.blocks.iter().find(|b| b.name.starts_with("blk1_")).unwrap();
        let blk2 = result.blocks.iter().find(|b| b.name.starts_with("blk2_")).unwrap();
        
        // Should choose DRMT for both blocks (fastest, no communication cost)
        assert_eq!(blk1.hw.name(), "DRMT");
        assert_eq!(blk2.hw.name(), "DRMT");
    }

    #[test]
    fn test_multi_block_communication_tradeoff() {
        let mut egraph = EGraphData::new();

        // Block 1: only CPU
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![BlockImpl {
                name: "blk1_cpu".to_string(),
                hw: Hardware::new("CPU".to_string()),
                ops: vec!["op1".to_string()],
            }],
        );

        // Block 2: CPU or DPA
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::min_latency());
        
        let cpu = Hardware::new("CPU".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        cost_model.set_op_cost("op1", &cpu, Cost::new(10.0, 2.0, 20.0));
        cost_model.set_op_cost("op2", &cpu, Cost::new(8.0, 3.0, 15.0));
        cost_model.set_op_cost("op2", &dpa, Cost::new(1.0, 20.0, 5.0)); // Very fast on DPA

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        
        let blk2 = result.blocks.iter().find(|b| b.name.starts_with("blk2_")).unwrap();
        
        // DPA is much faster (1.0) but with communication cost (50.0)
        // Total: 10 + 1 + 50 = 61 vs CPU: 10 + 8 = 18
        // CPU should win because communication cost is high
        assert_eq!(blk2.hw.name(), "CPU");
    }

    #[test]
    fn test_three_blocks_linear_chain() {
        let mut egraph = EGraphData::new();

        // Linear chain: blk1 -> blk2 -> blk3
        for i in 1..=3 {
            egraph.block_equivalences.insert(
                format!("blk{}", i),
                vec![
                    BlockImpl {
                        name: format!("blk{}_cpu", i),
                        hw: Hardware::new("CPU".to_string()),
                        ops: vec![format!("op{}", i)],
                    },
                    BlockImpl {
                        name: format!("blk{}_drmt", i),
                        hw: Hardware::new("DRMT".to_string()),
                        ops: vec![format!("op{}", i)],
                    },
                ],
            );
        }

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);
        egraph.dependencies.insert("blk3".to_string(), vec!["blk2".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        
        for i in 1..=3 {
            cost_model.set_op_cost(&format!("op{}", i), &cpu, Cost::new(10.0, 2.0, 20.0));
            cost_model.set_op_cost(&format!("op{}", i), &drmt, Cost::new(5.0, 5.0, 10.0));
        }

        for i in 1..=3 {
            cost_model.transfer_sizes.insert(format!("blk{}", i), 1.0);
        }

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        assert_eq!(result.blocks.len(), 3);
    }

    #[test]
    fn test_parallel_branches() {
        let mut egraph = EGraphData::new();

        // Fork: blk1 -> blk2
        //            -> blk3
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![BlockImpl {
                name: "blk1_cpu".to_string(),
                hw: Hardware::new("CPU".to_string()),
                ops: vec!["op1".to_string()],
            }],
        );

        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.block_equivalences.insert(
            "blk3".to_string(),
            vec![
                BlockImpl {
                    name: "blk3_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op3".to_string()],
                },
                BlockImpl {
                    name: "blk3_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op3".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);
        egraph.dependencies.insert("blk3".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        cost_model.set_op_cost("op1", &cpu, Cost::new(10.0, 2.0, 20.0));
        cost_model.set_op_cost("op2", &cpu, Cost::new(8.0, 3.0, 15.0));
        cost_model.set_op_cost("op2", &drmt, Cost::new(5.0, 5.0, 10.0));
        cost_model.set_op_cost("op3", &cpu, Cost::new(12.0, 2.5, 18.0));
        cost_model.set_op_cost("op3", &dpa, Cost::new(3.0, 10.0, 6.0));

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        assert_eq!(result.blocks.len(), 3);
    }

    #[test]
    fn test_pareto_front_with_tradeoffs() {
        let mut egraph = EGraphData::new();

        // Single block with three very different implementations
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        // CPU: slow, low throughput, low energy
        cost_model.set_op_cost("op1", &cpu, Cost::new(20.0, 2.0, 5.0));
        // DRMT: fast, medium throughput, high energy
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 25.0));
        // DPA: medium, high throughput, medium energy
        cost_model.set_op_cost("op1", &dpa, Cost::new(10.0, 10.0, 15.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let pareto = extractor.extract_pareto_front();

        // All three should be on Pareto front due to different tradeoffs
        assert_eq!(pareto.len(), 3);
    }

    #[test]
    fn test_dominated_solution() {
        let mut egraph = EGraphData::new();

        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        // CPU: worse in all dimensions
        cost_model.set_op_cost("op1", &cpu, Cost::new(20.0, 2.0, 30.0));
        // DPA: better in all dimensions
        cost_model.set_op_cost("op1", &dpa, Cost::new(5.0, 10.0, 8.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let pareto = extractor.extract_pareto_front();

        // Only DPA should be on Pareto front
        assert_eq!(pareto.len(), 1);
        assert_eq!(pareto[0].blocks[0].hw.name(), "DPA");
    }

    #[test]
    fn test_per_objective_extraction() {
        let mut egraph = EGraphData::new();

        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        // CPU: low latency, low throughput, high energy
        cost_model.set_op_cost("op1", &cpu, Cost::new(5.0, 2.0, 30.0));
        // DRMT: high latency, medium throughput, low energy
        cost_model.set_op_cost("op1", &drmt, Cost::new(20.0, 5.0, 8.0));
        // DPA: medium latency, high throughput, medium energy
        cost_model.set_op_cost("op1", &dpa, Cost::new(12.0, 10.0, 15.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let solutions = extractor.extract_per_objective();

        // Min latency should be CPU
        assert_eq!(solutions.min_latency.unwrap().blocks[0].hw.name(), "CPU");
        // Max throughput should be DPA
        assert_eq!(solutions.max_throughput.unwrap().blocks[0].hw.name(), "DPA");
        // Min energy should be DRMT
        assert_eq!(solutions.min_energy.unwrap().blocks[0].hw.name(), "DRMT");
    }

    #[test]
    fn test_complex_dependency_graph() {
        let mut egraph = EGraphData::new();

        // Complex graph:
        //     blk1 -> blk2 -> blk4
        //          -> blk3 -> blk4

        for i in 1..=4 {
            egraph.block_equivalences.insert(
                format!("blk{}", i),
                vec![
                    BlockImpl {
                        name: format!("blk{}_cpu", i),
                        hw: Hardware::new("CPU".to_string()),
                        ops: vec![format!("op{}", i)],
                    },
                    BlockImpl {
                        name: format!("blk{}_drmt", i),
                        hw: Hardware::new("DRMT".to_string()),
                        ops: vec![format!("op{}", i)],
                    },
                ],
            );
        }

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);
        egraph.dependencies.insert("blk3".to_string(), vec!["blk1".to_string()]);
        egraph.dependencies.insert("blk4".to_string(), vec!["blk2".to_string(), "blk3".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::min_latency());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        
        for i in 1..=4 {
            cost_model.set_op_cost(&format!("op{}", i), &cpu, Cost::new(10.0, 2.0, 20.0));
            cost_model.set_op_cost(&format!("op{}", i), &drmt, Cost::new(5.0, 5.0, 10.0));
            cost_model.transfer_sizes.insert(format!("blk{}", i), 1.0);
        }

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        assert_eq!(result.blocks.len(), 4);
    }

    #[test]
    fn test_no_implementations() {
        let egraph = EGraphData::new(); // Empty
        let topo = make_test_topology();
        let cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping();

        assert!(result.is_some());
        assert_eq!(result.unwrap().blocks.len(), 0);
    }

    #[test]
    fn test_single_implementation_per_block() {
        let mut egraph = EGraphData::new();

        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![BlockImpl {
                name: "blk1_cpu".to_string(),
                hw: Hardware::new("CPU".to_string()),
                ops: vec!["op1".to_string()],
            }],
        );

        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![BlockImpl {
                name: "blk2_cpu".to_string(),
                hw: Hardware::new("CPU".to_string()),
                ops: vec!["op2".to_string()],
            }],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        
        cost_model.set_op_cost("op1", &cpu, Cost::new(10.0, 2.0, 20.0));
        cost_model.set_op_cost("op2", &cpu, Cost::new(8.0, 3.0, 15.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        assert_eq!(result.blocks.len(), 2);
        
        let blk1 = result.blocks.iter().find(|b| b.name.starts_with("blk1_")).unwrap();
        let blk2 = result.blocks.iter().find(|b| b.name.starts_with("blk2_")).unwrap();
        
        assert_eq!(blk1.hw.name(), "CPU");
        assert_eq!(blk2.hw.name(), "CPU");
    }

    #[test]
    fn test_multiple_impls_with_limited_connectivity() {
        let mut egraph = EGraphData::new();

        // Block 1: DPA or DRMT implementations
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        // Block 2: DPA or CPU implementations, depends on blk1
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::min_latency());
        
        let dpa = Hardware::new("DPA".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let cpu = Hardware::new("CPU".to_string());
        
        // Set operation costs
        cost_model.set_op_cost("op1", &dpa, Cost::new(5.0, 10.0, 10.0));
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 10.0));
        cost_model.set_op_cost("op2", &dpa, Cost::new(3.0, 10.0, 8.0));
        cost_model.set_op_cost("op2", &cpu, Cost::new(10.0, 2.0, 20.0));

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        
        let blk1 = result.blocks.iter().find(|b| b.name.starts_with("blk1_")).unwrap();
        let blk2 = result.blocks.iter().find(|b| b.name.starts_with("blk2_")).unwrap();
        
        // Valid paths:
        // 1. DPA -> DPA: total latency = 5 + 3 + 0 (no comm) = 8
        // 2. DRMT -> CPU: total latency = 5 + 10 + 10 (comm) = 25
        // Invalid paths:
        // - DPA -> CPU: would have comm cost 50
        // - DRMT -> DPA: disconnected (infinite cost)
        
        // Should choose DPA -> DPA (minimum latency)
        assert_eq!(blk1.hw.name(), "DPA");
        assert_eq!(blk2.hw.name(), "DPA");
        assert_eq!(result.total_cost.communication, 0.0);
        assert_eq!(result.total_cost.total_latency(), 8.0);
    }

    #[test]
    fn test_forced_path_due_to_connectivity() {
        let mut egraph = EGraphData::new();

        // Block 1: only DRMT implementation
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        // Block 2: DPA or CPU implementations, depends on blk1
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::min_latency());
        
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        let cpu = Hardware::new("CPU".to_string());
        
        // Make DPA faster but unreachable from DRMT
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 10.0));
        cost_model.set_op_cost("op2", &dpa, Cost::new(1.0, 20.0, 5.0)); // Very fast
        cost_model.set_op_cost("op2", &cpu, Cost::new(10.0, 2.0, 20.0));

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        
        let blk1 = result.blocks.iter().find(|b| b.name.starts_with("blk1_")).unwrap();
        let blk2 = result.blocks.iter().find(|b| b.name.starts_with("blk2_")).unwrap();
        
        // Even though DPA is much faster (1.0 vs 10.0), DRMT->DPA is disconnected
        // So must use DRMT -> CPU path
        assert_eq!(blk1.hw.name(), "DRMT");
        assert_eq!(blk2.hw.name(), "CPU");
        
        // Communication: DRMT -> CPU = 10.0
        assert_eq!(result.total_cost.communication, 10.0);
    }

    #[test]
    fn test_three_blocks_connectivity_constraints() {
        let mut egraph = EGraphData::new();

        // Block 1: CPU or DRMT
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        // Block 2: DRMT or DPA, depends on blk1
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        // Block 3: CPU or DPA, depends on blk2
        egraph.block_equivalences.insert(
            "blk3".to_string(),
            vec![
                BlockImpl {
                    name: "blk3_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op3".to_string()],
                },
                BlockImpl {
                    name: "blk3_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op3".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);
        egraph.dependencies.insert("blk3".to_string(), vec!["blk2".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::min_latency());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        // All operations have same cost
        for hw in [cpu, drmt, dpa] {
            cost_model.set_op_cost("op1", &hw, Cost::new(5.0, 5.0, 10.0));
            cost_model.set_op_cost("op2", &hw, Cost::new(5.0, 5.0, 10.0));
            cost_model.set_op_cost("op3", &hw, Cost::new(5.0, 5.0, 10.0));
        }

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);
        cost_model.transfer_sizes.insert("blk2".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping().unwrap();

        assert!(result.is_feasible);
        assert_eq!(result.blocks.len(), 3);
        
        // Find blocks by name instead of assuming order
        let blk1 = result.blocks.iter().find(|b| b.name.starts_with("blk1_")).unwrap();
        let blk2 = result.blocks.iter().find(|b| b.name.starts_with("blk2_")).unwrap();
        let blk3 = result.blocks.iter().find(|b| b.name.starts_with("blk3_")).unwrap();
        
        // Verify block 2 is using a valid implementation (DRMT or DPA, not CPU)
        assert!(
            blk2.hw.name() == "DRMT" || blk2.hw.name() == "DPA",
            "blk2 should be DRMT or DPA, got {:?}",
            blk2.hw.name()
        );
        
        // Verify all connections are valid
        assert!(cost_model.is_connected(&blk1.hw, &blk2.hw),
            "blk1({:?}) -> blk2({:?}) should be connected",
            blk1.hw.name(), blk2.hw.name());
        
        assert!(cost_model.is_connected(&blk2.hw, &blk3.hw),
            "blk2({:?}) -> blk3({:?}) should be connected",
            blk2.hw.name(), blk3.hw.name());
        
        // Verify it's not using the disconnected DRMT<->DPA path
        assert!(
            !(blk1.hw.name() == "DRMT" && blk2.hw.name() == "DPA"),
            "Should not have DRMT->DPA path (disconnected)"
        );
        assert!(
            !(blk2.hw.name() == "DRMT" && blk3.hw.name() == "DPA"),
            "Should not have DRMT->DPA path (disconnected)"
        );
        assert!(
            !(blk1.hw.name() == "DPA" && blk2.hw.name() == "DRMT"),
            "Should not have DPA->DRMT path (disconnected)"
        );
        assert!(
            !(blk2.hw.name() == "DPA" && blk3.hw.name() == "DRMT"),
            "Should not have DPA->DRMT path (disconnected)"
        );
    }

    #[test]
    fn test_all_paths_blocked() {
        let mut egraph = EGraphData::new();

        // Block 1: only DRMT
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        // Block 2: only DPA, depends on blk1
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 10.0));
        cost_model.set_op_cost("op2", &dpa, Cost::new(3.0, 10.0, 8.0));

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let result = extractor.extract_best_mapping();

        // No feasible mapping because DRMT->DPA is disconnected
        assert!(result.is_none());
    }

    #[test]
    fn test_pareto_front_with_connectivity() {
        let mut egraph = EGraphData::new();

        // Block 1: CPU or DRMT
        egraph.block_equivalences.insert(
            "blk1".to_string(),
            vec![
                BlockImpl {
                    name: "blk1_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op1".to_string()],
                },
                BlockImpl {
                    name: "blk1_drmt".to_string(),
                    hw: Hardware::new("DRMT".to_string()),
                    ops: vec!["op1".to_string()],
                },
            ],
        );

        // Block 2: CPU or DPA, depends on blk1
        egraph.block_equivalences.insert(
            "blk2".to_string(),
            vec![
                BlockImpl {
                    name: "blk2_cpu".to_string(),
                    hw: Hardware::new("CPU".to_string()),
                    ops: vec!["op2".to_string()],
                },
                BlockImpl {
                    name: "blk2_dpa".to_string(),
                    hw: Hardware::new("DPA".to_string()),
                    ops: vec!["op2".to_string()],
                },
            ],
        );

        egraph.dependencies.insert("blk1".to_string(), vec![]);
        egraph.dependencies.insert("blk2".to_string(), vec!["blk1".to_string()]);

        let topo = make_test_topology();
        let mut cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());
        
        let cpu = Hardware::new("CPU".to_string());
        let drmt = Hardware::new("DRMT".to_string());
        let dpa = Hardware::new("DPA".to_string());
        
        // CPU: slow, low energy
        cost_model.set_op_cost("op1", &cpu, Cost::new(20.0, 2.0, 5.0));
        cost_model.set_op_cost("op2", &cpu, Cost::new(15.0, 2.0, 5.0));
        
        // DRMT: fast, high energy
        cost_model.set_op_cost("op1", &drmt, Cost::new(5.0, 5.0, 25.0));
        
        // DPA: very fast, medium energy
        cost_model.set_op_cost("op2", &dpa, Cost::new(2.0, 10.0, 15.0));

        cost_model.transfer_sizes.insert("blk1".to_string(), 1.0);

        let extractor = BlockExtractor::new(&egraph, &cost_model);
        let pareto = extractor.extract_pareto_front();

        // Should have multiple solutions on Pareto front
        assert!(pareto.len() >= 2);
        assert!(pareto.iter().all(|m| m.is_feasible));
        
        // Verify no infeasible paths made it through
        for mapping in &pareto {
            for i in 0..mapping.blocks.len() {
                for dep in &mapping.blocks[i].deps {
                    let dep_block_id = mapping.blocks[i].deps.first();
                    if let Some(dep_id) = dep_block_id {
                        if let Some(dep_block) = mapping.blocks.iter().find(|b| {
                            let block_id = b.name.split('_').next().unwrap_or(&b.name);
                            block_id == dep_id
                        }) {
                            assert!(cost_model.is_connected(&dep_block.hw, &mapping.blocks[i].hw),
                                "Found disconnected path: {:?} -> {:?}", 
                                dep_block.hw.name(), mapping.blocks[i].hw.name());
                        }
                    }
                }
            }
        }
    }
}