// src/cost_model.rs
use std::collections::HashMap;
use std::fmt;

use crate::dataflow::{Hardware, HardwareTopology};

/// Multi-objective cost with three dimensions
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Cost {
    /// Latency: time to complete (cycles, ms) - LOWER is better
    pub latency: f64,
    
    /// Throughput: operations per unit time - HIGHER is better
    /// We store 1/throughput so lower is better for consistency
    pub inverse_throughput: f64,
    
    /// Energy: total energy consumption (joules) - LOWER is better
    pub energy: f64,
    
    /// Communication cost (latency component)
    pub communication: f64,
}

impl Cost {
    pub fn new(latency: f64, throughput: f64, energy: f64) -> Self {
        Self {
            latency,
            inverse_throughput: if throughput > 0.0 { 1.0 / throughput } else { f64::INFINITY },
            energy,
            communication: 0.0,
        }
    }

    #[allow(dead_code)]
    pub fn with_comm(latency: f64, throughput: f64, energy: f64, communication: f64) -> Self {
        Self {
            latency,
            inverse_throughput: if throughput > 0.0 { 1.0 / throughput } else { f64::INFINITY },
            energy,
            communication,
        }
    }

    pub fn zero() -> Self {
        Self {
            latency: 0.0,
            inverse_throughput: 0.0,
            energy: 0.0,
            communication: 0.0,
        }
    }

    pub fn infinite() -> Self {
        Self {
            latency: f64::INFINITY,
            inverse_throughput: f64::INFINITY,
            energy: f64::INFINITY,
            communication: f64::INFINITY,
        }
    }

    /// Get actual throughput (inverse of inverse_throughput)
    pub fn throughput(&self) -> f64 {
        if self.inverse_throughput > 0.0 {
            1.0 / self.inverse_throughput
        } else {
            f64::INFINITY
        }
    }

    /// Total latency including communication
    pub fn total_latency(&self) -> f64 {
        self.latency + self.communication
    }

    /// Check if feasible
    pub fn is_feasible(&self) -> bool {
        self.latency.is_finite()
            && self.inverse_throughput.is_finite()
            && self.energy.is_finite()
            && self.communication.is_finite()
    }

    /// Add two costs (for sequential composition)
    pub fn add_sequential(&self, other: &Cost) -> Cost {
        Cost {
            latency: self.latency + other.latency,
            // Sequential: throughput limited by slowest stage
            inverse_throughput: self.inverse_throughput.max(other.inverse_throughput),
            energy: self.energy + other.energy,
            communication: self.communication + other.communication,
        }
    }

    /// Add costs for parallel composition
    #[allow(dead_code)]
    pub fn add_parallel(&self, other: &Cost) -> Cost {
        Cost {
            // Parallel: latency is max of both paths
            latency: self.latency.max(other.latency),
            // Parallel: throughput is min of both (bottleneck)
            inverse_throughput: self.inverse_throughput.max(other.inverse_throughput),
            // Energy: sum of both
            energy: self.energy + other.energy,
            communication: self.communication + other.communication,
        }
    }

    /// Pareto dominance: better in all objectives
    pub fn dominates(&self, other: &Cost) -> bool {
        let better_latency = self.total_latency() < other.total_latency();
        let better_throughput = self.inverse_throughput < other.inverse_throughput;
        let better_energy = self.energy < other.energy;

        let not_worse_latency = self.total_latency() <= other.total_latency();
        let not_worse_throughput = self.inverse_throughput <= other.inverse_throughput;
        let not_worse_energy = self.energy <= other.energy;

        (better_latency || better_throughput || better_energy)
            && not_worse_latency
            && not_worse_throughput
            && not_worse_energy
    }

    /// Weighted sum for single-objective optimization
    pub fn weighted_sum(&self, weights: &ObjectiveWeights) -> f64 {
        if !self.is_feasible() {
            return f64::INFINITY;
        }
        weights.latency * self.total_latency()
            + weights.throughput * self.inverse_throughput
            + weights.energy * self.energy
    }
}

impl fmt::Display for Cost {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "(lat: {:.2}, tput: {:.2}, energy: {:.2}, comm: {:.2})",
            self.latency,
            self.throughput(),
            self.energy,
            self.communication
        )
    }
}

/// Weights for multi-objective optimization
#[derive(Debug, Clone, Copy)]
pub struct ObjectiveWeights {
    pub latency: f64,
    pub throughput: f64,
    pub energy: f64,
}

impl ObjectiveWeights {
    /// Optimize for minimum latency
    pub fn min_latency() -> Self {
        Self {
            latency: 1.0,
            throughput: 0.0,
            energy: 0.0,
        }
    }

    /// Optimize for maximum throughput
    pub fn max_throughput() -> Self {
        Self {
            latency: 0.0,
            throughput: 1.0,
            energy: 0.0,
        }
    }

    /// Optimize for minimum energy
    pub fn min_energy() -> Self {
        Self {
            latency: 0.0,
            throughput: 0.0,
            energy: 1.0,
        }
    }

    /// Balanced optimization
    pub fn balanced() -> Self {
        Self {
            latency: 1.0,
            throughput: 1.0,
            energy: 1.0,
        }
    }

    /// Custom weights
    #[allow(dead_code)]
    pub fn custom(latency: f64, throughput: f64, energy: f64) -> Self {
        Self {
            latency,
            throughput,
            energy,
        }
    }
}

/// Cost model with three objectives
pub struct CostModel {
    /// Operation costs: (op_name, hardware) -> Cost
    pub op_costs: HashMap<(String, String), Cost>,

    /// Hardware topology (defines communication costs)
    pub topology: HardwareTopology,

    /// Data transfer sizes
    pub transfer_sizes: HashMap<String, f64>,

    /// Optimization weights (for single-objective)
    pub weights: ObjectiveWeights,
}

impl CostModel {
    pub fn from_topology(topology: HardwareTopology, weights: ObjectiveWeights) -> Self {
        Self {
            op_costs: HashMap::new(),
            topology,
            transfer_sizes: HashMap::new(),
            weights,
        }
    }

    pub fn set_op_cost(&mut self, op_name: &str, hw: &Hardware, cost: Cost) {
        self.op_costs.insert((op_name.to_string(), hw.name().to_string()), cost);
    }

    pub fn get_op_cost(&self, op_name: &str, hw: &Hardware) -> Cost {
        self.op_costs
            .get(&(op_name.to_string(), hw.name().to_string()))
            .copied()
            .unwrap_or_else(|| Cost::new(1.0, 1.0, 1.0))
    }

    pub fn get_comm_cost(&self, from_hw: &Hardware, to_hw: &Hardware, data_size: f64) -> f64 {
        // Same hardware - zero cost
        if from_hw.name() == to_hw.name() {
            return 0.0;
        }

        // Look up communication link
        if let Some(link) = self.topology.get_link(from_hw.name(), to_hw.name()) {
            link.latency_per_unit * data_size
        } else {
            // No link defined - infinite cost (disconnected)
            f64::INFINITY
        }
    }

    pub fn is_connected(&self, from_hw: &Hardware, to_hw: &Hardware) -> bool {
        self.topology.is_connected(from_hw.name(), to_hw.name())
    }

    pub fn compute_block_cost(&self, ops: &[String], hw: &Hardware) -> Cost {
        let mut total = Cost::zero();
        for op in ops {
            let op_cost = self.get_op_cost(op, hw);
            // Sequential within a block
            total = total.add_sequential(&op_cost);
        }
        total
    }
}

/// Block with computed costs
#[derive(Debug, Clone)]
pub struct CostedBlock {
    pub name: String,
    pub hw: Hardware,
    pub ops: Vec<String>,
    pub deps: Vec<String>,
    pub cost: Cost,
}

impl CostedBlock {
    pub fn new(
        name: String,
        hw: Hardware,
        ops: Vec<String>,
        deps: Vec<String>,
        cost_model: &CostModel,
    ) -> Self {
        let cost = cost_model.compute_block_cost(&ops, &hw);
        Self {
            name,
            hw,
            ops,
            deps,
            cost,
        }
    }
}

/// Complete mapping with multi-objective costs
#[derive(Debug, Clone)]
pub struct BlockMapping {
    pub blocks: Vec<CostedBlock>,
    pub total_cost: Cost,
    pub is_feasible: bool,
}

impl BlockMapping {
    pub fn new() -> Self {
        Self {
            blocks: Vec::new(),
            total_cost: Cost::zero(),
            is_feasible: true,
        }
    }
    
    pub fn compute_total_cost(&mut self, cost_model: &CostModel) {
        if self.blocks.is_empty() {
            self.is_feasible = true;
            self.total_cost = Cost::zero();
            return;
        }

        // Build a map from block ID to CostedBlock for dependency lookup
        let mut block_map: HashMap<String, &CostedBlock> = HashMap::new();
        for block in &self.blocks {
            // Extract block ID from implementation name (e.g., "blk1_cpu" -> "blk1")
            let block_id = self.extract_block_id(&block.name);
            block_map.insert(block_id.to_string(), block);
        }

        // Compute costs considering dataflow structure
        let mut block_costs: HashMap<String, Cost> = HashMap::new();
        let mut feasible = true;

        // Topological sort to compute costs in dependency order
        let sorted = self.topological_sort();

        for block_name in &sorted {
            if let Some(block) = self.blocks.iter().find(|b| {
                let block_id = self.extract_block_id(&b.name);
                block_id == *block_name
            }) {
                let mut total_cost = block.cost;

                // Add communication costs from dependencies
                for dep_block_id in &block.deps {
                    if let Some(dep_block) = block_map.get(dep_block_id) {
                        let data_size = cost_model
                            .transfer_sizes
                            .get(dep_block_id)
                            .copied()
                            .unwrap_or(1.0);

                        let comm_cost = cost_model.get_comm_cost(&dep_block.hw, &block.hw, data_size);

                        if comm_cost.is_infinite() {
                            feasible = false;
                            break;
                        }

                        total_cost.communication += comm_cost;
                    }
                }

                if !feasible {
                    break;
                }

                let block_id = self.extract_block_id(&block.name);
                block_costs.insert(block_id.to_string(), total_cost);
            }
        }

        if !feasible {
            self.is_feasible = false;
            self.total_cost = Cost::infinite();
            return;
        }

        // Compute final cost (depends on graph structure)
        // For now, assume sequential execution (conservative)
        let mut total = Cost::zero();
        for block_name in &sorted {
            if let Some(cost) = block_costs.get(block_name) {
                total = total.add_sequential(cost);
            }
        }

        self.total_cost = total;
        self.is_feasible = true;
    }

    /// Extract block ID from implementation name (e.g., "blk1_cpu" -> "blk1")
    fn extract_block_id<'a>(&self, impl_name: &'a str) -> &'a str {
        impl_name.split('_').next().unwrap_or(impl_name)
    }

    /// Topological sort of blocks (by block ID, not implementation name)
    fn topological_sort(&self) -> Vec<String> {
        let mut visited = std::collections::HashSet::new();
        let mut result = Vec::new();

        // Build dependency map by block ID
        let mut dep_map: HashMap<String, Vec<String>> = HashMap::new();
        for block in &self.blocks {
            let block_id = self.extract_block_id(&block.name);
            dep_map.insert(block_id.to_string(), block.deps.clone());
        }

        fn visit(
            block_id: &str,
            dep_map: &HashMap<String, Vec<String>>,
            visited: &mut std::collections::HashSet<String>,
            result: &mut Vec<String>,
        ) {
            if visited.contains(block_id) {
                return;
            }
            visited.insert(block_id.to_string());

            if let Some(deps) = dep_map.get(block_id) {
                for dep in deps {
                    visit(dep, dep_map, visited, result);
                }
            }

            result.push(block_id.to_string());
        }

        for block in &self.blocks {
            let block_id = self.extract_block_id(&block.name);
            visit(block_id, &dep_map, &mut visited, &mut result);
        }

        result
    }

    pub fn weighted_cost(&self, cost_model: &CostModel) -> f64 {
        self.total_cost.weighted_sum(&cost_model.weights)
    }
}

impl Default for BlockMapping {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cost_creation() {
        let cost = Cost::new(10.0, 5.0, 3.0);
        assert_eq!(cost.latency, 10.0);
        assert_eq!(cost.throughput(), 5.0);
        assert_eq!(cost.energy, 3.0);
    }

    #[test]
    fn test_hardware_topology_in_cost_model() {
        let mut topo = HardwareTopology::new();
        topo.add_hardware("CPU".to_string());
        topo.add_hardware("GPU".to_string());
        topo.add_link("CPU".to_string(), "GPU".to_string(), 10.0, 1.0);

        let cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());

        let cpu = Hardware::new("CPU".to_string());
        let gpu = Hardware::new("GPU".to_string());

        // Same hardware - zero cost
        assert_eq!(cost_model.get_comm_cost(&cpu, &cpu, 1.0), 0.0);
        
        // Connected path
        assert_eq!(cost_model.get_comm_cost(&cpu, &gpu, 1.0), 10.0);
        assert_eq!(cost_model.get_comm_cost(&cpu, &gpu, 2.0), 20.0);
        
        // Disconnected (no link defined for GPU->CPU)
        assert!(cost_model.get_comm_cost(&gpu, &cpu, 1.0).is_infinite());
    }

    #[test]
    fn test_is_connected() {
        let mut topo = HardwareTopology::new();
        topo.add_hardware("CPU".to_string());
        topo.add_hardware("GPU".to_string());
        topo.add_link("CPU".to_string(), "GPU".to_string(), 10.0, 1.0);

        let cost_model = CostModel::from_topology(topo, ObjectiveWeights::balanced());

        let cpu = Hardware::new("CPU".to_string());
        let gpu = Hardware::new("GPU".to_string());

        assert!(cost_model.is_connected(&cpu, &cpu));
        assert!(cost_model.is_connected(&cpu, &gpu));
        assert!(!cost_model.is_connected(&gpu, &cpu));
    }
}