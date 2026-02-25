// src/cost_model.rs
use std::collections::HashMap;
use std::fmt;

use crate::dataflow::{Hardware, HardwareTopology};

/// Operation type for modeling parallelism
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpType {
    Stateless,  // Independent operations (default)
    Stateful,   // Shared-resource operations
}

impl Default for OpType {
    fn default() -> Self {
        OpType::Stateless
    }
}

/// Multi-objective cost with three dimensions
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Cost {
    pub latency: f64,
    pub inverse_throughput: f64,
    pub energy: f64,
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

    pub fn throughput(&self) -> f64 {
        if self.inverse_throughput > 0.0 {
            1.0 / self.inverse_throughput
        } else {
            f64::INFINITY
        }
    }

    pub fn total_latency(&self) -> f64 {
        self.latency + self.communication
    }

    pub fn is_feasible(&self) -> bool {
        self.latency.is_finite()
            && self.inverse_throughput.is_finite()
            && self.energy.is_finite()
            && self.communication.is_finite()
    }

    pub fn add_sequential(&self, other: &Cost) -> Cost {
        Cost {
            latency: self.latency + other.latency,
            inverse_throughput: self.inverse_throughput.max(other.inverse_throughput),
            energy: self.energy + other.energy,
            communication: self.communication + other.communication,
        }
    }

    pub fn add_parallel(&self, other: &Cost) -> Cost {
        Cost {
            latency: self.latency.max(other.latency),
            inverse_throughput: self.inverse_throughput.max(other.inverse_throughput),
            energy: self.energy + other.energy,
            communication: self.communication + other.communication,
        }
    }

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

/// Detailed operation cost with type information
#[derive(Debug, Clone, Copy)]
pub struct OpCost {
    pub latency: f64,
    pub energy: f64,
    pub throughput: f64,
    pub op_type: OpType,
    pub sigma: f64,  // Contention factor for stateful ops (only used if op_type == Stateful)
}

impl OpCost {
    pub fn new_stateless(latency: f64, energy: f64, throughput: f64) -> Self {
        Self {
            latency,
            energy,
            throughput,
            op_type: OpType::Stateless,
            sigma: 0.0,
        }
    }

    pub fn new_stateful(latency: f64, energy: f64, throughput: f64, sigma: f64) -> Self {
        Self {
            latency,
            energy,
            throughput,
            op_type: OpType::Stateful,
            sigma,
        }
    }
}

/// Hardware configuration with parallelism parameters
#[derive(Debug, Clone)]
pub struct HardwareConfig {
    pub num_cores: usize,
    pub default_sigma: f64,  // Default contention factor for this hardware
}

impl HardwareConfig {
    pub fn new(num_cores: usize, default_sigma: f64) -> Self {
        Self {
            num_cores,
            default_sigma,
        }
    }

    pub fn single_core() -> Self {
        Self {
            num_cores: 1,
            default_sigma: 0.0,
        }
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
    pub fn min_latency() -> Self {
        Self {
            latency: 1.0,
            throughput: 0.0,
            energy: 0.0,
        }
    }

    pub fn max_throughput() -> Self {
        Self {
            latency: 0.0,
            throughput: 1.0,
            energy: 0.0,
        }
    }

    pub fn min_energy() -> Self {
        Self {
            latency: 0.0,
            throughput: 0.0,
            energy: 1.0,
        }
    }

    pub fn balanced() -> Self {
        Self {
            latency: 1.0,
            throughput: 1.0,
            energy: 1.0,
        }
    }

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
    /// Detailed operation costs: (op_name, hardware) -> OpCost
    pub op_costs: HashMap<(String, String), OpCost>,

    /// Hardware configurations: hardware_name -> HardwareConfig
    pub hw_configs: HashMap<String, HardwareConfig>,

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
            hw_configs: HashMap::new(),
            topology,
            transfer_sizes: HashMap::new(),
            weights,
        }
    }

    /// Set detailed operation cost with type information
    pub fn set_op_cost_detailed(&mut self, op_name: &str, hw: &Hardware, op_cost: OpCost) {
        self.op_costs.insert((op_name.to_string(), hw.name().to_string()), op_cost);
    }

    /// Legacy method for backward compatibility
    pub fn set_op_cost(&mut self, op_name: &str, hw: &Hardware, cost: Cost) {
        let op_cost = OpCost::new_stateless(cost.latency, cost.energy, cost.throughput());
        self.set_op_cost_detailed(op_name, hw, op_cost);
    }

    /// Get detailed operation cost
    pub fn get_op_cost_detailed(&self, op_name: &str, hw: &Hardware) -> OpCost {
        self.op_costs
            .get(&(op_name.to_string(), hw.name().to_string()))
            .copied()
            .unwrap_or_else(|| OpCost::new_stateless(1.0, 1.0, 1.0))
    }

    /// Legacy method - returns simplified Cost
    pub fn get_op_cost(&self, op_name: &str, hw: &Hardware) -> Cost {
        let op_cost = self.get_op_cost_detailed(op_name, hw);
        Cost::new(op_cost.latency, op_cost.throughput, op_cost.energy)
    }

    /// Set hardware configuration
    pub fn set_hw_config(&mut self, hw_name: &str, config: HardwareConfig) {
        self.hw_configs.insert(hw_name.to_string(), config);
    }

    /// Get hardware configuration (default: single core)
    pub fn get_hw_config(&self, hw: &Hardware) -> HardwareConfig {
        self.hw_configs
            .get(hw.name())
            .cloned()
            .unwrap_or_else(HardwareConfig::single_core)
    }

    pub fn get_comm_cost(&self, from_hw: &Hardware, to_hw: &Hardware, data_size: f64) -> f64 {
        if from_hw.name() == to_hw.name() {
            return 0.0;
        }

        if let Some(link) = self.topology.get_link(from_hw.name(), to_hw.name()) {
            link.latency_per_unit * data_size
        } else {
            f64::INFINITY
        }
    }

    pub fn is_connected(&self, from_hw: &Hardware, to_hw: &Hardware) -> bool {
        self.topology.is_connected(from_hw.name(), to_hw.name())
    }

    /// Compute block cost using paper's formula:
    /// L_Comp = Cost_indep + Cost_shared × (1 + σ(P-1))
    /// T_Comp = P / L_Comp
    pub fn compute_block_cost(&self, ops: &[String], hw: &Hardware) -> Cost {
        let hw_config = self.get_hw_config(hw);
        let P = hw_config.num_cores as f64;

        let mut cost_indep = 0.0;
        let mut cost_shared = 0.0;
        let mut energy_indep = 0.0;
        let mut energy_shared = 0.0;
        let mut max_sigma: f64 = 0.0;

        for op_name in ops {
            let op_cost = self.get_op_cost_detailed(op_name, hw);

            match op_cost.op_type {
                OpType::Stateless => {
                    cost_indep += op_cost.latency;
                    energy_indep += op_cost.energy;
                }
                OpType::Stateful => {
                    cost_shared += op_cost.latency;
                    energy_shared += op_cost.energy;
                    // Use max sigma among all stateful ops in block
                    max_sigma = max_sigma.max(op_cost.sigma);
                }
            }
        }

        // If no stateful ops have sigma set, use hardware default
        if max_sigma == 0.0 && cost_shared > 0.0 {
            max_sigma = hw_config.default_sigma;
        }

        // Paper's formula: L_Comp = Cost_indep + Cost_shared × (1 + σ(P-1))
        let total_latency = cost_indep + cost_shared * (1.0 + max_sigma * (P - 1.0));

        // Throughput: T_Comp = P / L_Comp
        let throughput = if total_latency > 0.0 {
            P / total_latency
        } else {
            f64::INFINITY
        };

        // Energy: simple sum (no multi-core overhead for now)
        let total_energy = energy_indep + energy_shared;

        Cost::new(total_latency, throughput, total_energy)
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

        let mut block_map: HashMap<String, &CostedBlock> = HashMap::new();
        for block in &self.blocks {
            let block_id = self.extract_block_id(&block.name);
            block_map.insert(block_id.to_string(), block);
        }

        let mut block_costs: HashMap<String, Cost> = HashMap::new();
        let mut feasible = true;

        let sorted = self.topological_sort();

        for block_name in &sorted {
            if let Some(block) = self.blocks.iter().find(|b| {
                let block_id = self.extract_block_id(&b.name);
                block_id == *block_name
            }) {
                let mut total_cost = block.cost;

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

        let mut total = Cost::zero();
        for block_name in &sorted {
            if let Some(cost) = block_costs.get(block_name) {
                total = total.add_sequential(cost);
            }
        }

        self.total_cost = total;
        self.is_feasible = true;
    }

    fn extract_block_id<'a>(&self, impl_name: &'a str) -> &'a str {
        impl_name.split('_').next().unwrap_or(impl_name)
    }

    fn topological_sort(&self) -> Vec<String> {
        let mut visited = std::collections::HashSet::new();
        let mut result = Vec::new();

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
    fn test_stateless_parallelism() {
        let mut cost_model = CostModel::from_topology(
            HardwareTopology::new(),
            ObjectiveWeights::balanced(),
        );

        let hw = Hardware::new("DPA".to_string());
        cost_model.set_hw_config("DPA", HardwareConfig::new(8, 0.5));

        // Two stateless ops, 10 cycles each
        cost_model.set_op_cost_detailed(
            "hash",
            &hw,
            OpCost::new_stateless(10.0, 5.0, 1.0),
        );

        let cost = cost_model.compute_block_cost(&vec!["hash".to_string(), "hash".to_string()], &hw);

        // Cost_indep = 20, Cost_shared = 0
        // L_Comp = 20 + 0 = 20 cycles
        // T_Comp = 8 / 20 = 0.4 ops/cycle
        assert_eq!(cost.latency, 20.0);
        assert!((cost.throughput() - 0.4).abs() < 0.01);
    }

    #[test]
    fn test_stateful_contention() {
        let mut cost_model = CostModel::from_topology(
            HardwareTopology::new(),
            ObjectiveWeights::balanced(),
        );

        let hw = Hardware::new("DPA".to_string());
        cost_model.set_hw_config("DPA", HardwareConfig::new(8, 0.5));

        // Stateful op with sigma=1.0
        cost_model.set_op_cost_detailed(
            "table_lookup",
            &hw,
            OpCost::new_stateful(20.0, 10.0, 1.0, 1.0),
        );

        let cost = cost_model.compute_block_cost(&vec!["table_lookup".to_string()], &hw);

        // Cost_shared = 20, P = 8, sigma = 1.0
        // L_Comp = 0 + 20 × (1 + 1.0 × (8-1)) = 20 × 8 = 160 cycles
        // T_Comp = 8 / 160 = 0.05 ops/cycle
        assert_eq!(cost.latency, 160.0);
        assert!((cost.throughput() - 0.05).abs() < 0.01);
    }

    #[test]
    fn test_mixed_workload() {
        let mut cost_model = CostModel::from_topology(
            HardwareTopology::new(),
            ObjectiveWeights::balanced(),
        );

        let hw = Hardware::new("DPA".to_string());
        cost_model.set_hw_config("DPA", HardwareConfig::new(4, 0.5));

        cost_model.set_op_cost_detailed(
            "hash",
            &hw,
            OpCost::new_stateless(10.0, 5.0, 1.0),
        );
        cost_model.set_op_cost_detailed(
            "lookup",
            &hw,
            OpCost::new_stateful(30.0, 15.0, 1.0, 0.8),
        );

        let cost = cost_model.compute_block_cost(
            &vec!["hash".to_string(), "lookup".to_string()],
            &hw,
        );

        // Cost_indep = 10, Cost_shared = 30, P = 4, sigma = 0.8
        // L_Comp = 10 + 30 × (1 + 0.8 × 3) = 10 + 30 × 3.4 = 112 cycles
        // T_Comp = 4 / 112 = 0.0357 ops/cycle
        assert_eq!(cost.latency, 112.0);
        assert!((cost.throughput() - 0.0357).abs() < 0.001);
    }
}