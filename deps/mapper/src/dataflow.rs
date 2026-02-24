// src/dataflow.rs
use std::collections::HashMap;
use std::fmt;

// Use Literal from egglog-ast
pub use egglog_ast::generic_ast::Literal;

/// Hardware type - a string identifier for a hardware unit
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Hardware(pub String);

impl fmt::Display for Hardware {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl Hardware {
    pub fn new(name: String) -> Self {
        Hardware(name)
    }

    pub fn name(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Clone)]
pub struct BlockImpl {
    pub name: String,
    pub hardware: Hardware,
    pub deps: Vec<String>,
    pub ops: Vec<Operation>,
}

#[derive(Debug, Clone)]
pub struct Operation {
    #[allow(dead_code)]
    pub var_name: String,
    pub expr: OpExpr,
}

#[derive(Debug, Clone)]
pub enum OpExpr {
    Var(String),
    #[allow(dead_code)]
    Literal(Literal),
    Call {
        func: String,
        #[allow(dead_code)]
        args: Vec<OpArg>,
    },
}

#[derive(Debug, Clone)]
pub enum OpArg {
    #[allow(dead_code)]
    Var(String),
    #[allow(dead_code)]
    Literal(Literal),
    #[allow(dead_code)]
    Nested(Box<OpExpr>),
}

#[derive(Debug, Clone)]
pub struct Block {
    #[allow(dead_code)]
    pub id: String,
    pub implementations: Vec<BlockImpl>,
}

// Pattern for rewrite rules
#[derive(Debug, Clone, PartialEq)]
pub enum Pattern {
    Impl(String, Hardware),
    Var(String),
}

impl fmt::Display for Pattern {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Pattern::Impl(name, hw) => write!(f, "{}@{}", name, hw),
            Pattern::Var(v) => write!(f, "?{}", v),
        }
    }
}

#[derive(Debug, Clone)]
pub struct RewriteRule {
    pub lhs: Pattern,
    pub rhs: Pattern,
}

impl fmt::Display for RewriteRule {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} => {}", self.lhs, self.rhs)
    }
}

/// Hardware topology definition
#[derive(Debug, Clone)]
pub struct HardwareTopology {
    /// List of available hardware types
    pub hardware_types: Vec<String>,
    
    /// Communication links: (from, to) -> (latency_per_unit, bandwidth_factor)
    /// latency_per_unit: base communication cost per unit of data
    /// bandwidth_factor: affects throughput (lower is better)
    pub links: HashMap<(String, String), CommunicationLink>,
}

#[derive(Debug, Clone, Copy)]
pub struct CommunicationLink {
    pub latency_per_unit: f64,
    pub bandwidth_factor: f64,  // For future use in throughput calculations
}

impl HardwareTopology {
    pub fn new() -> Self {
        Self {
            hardware_types: Vec::new(),
            links: HashMap::new(),
        }
    }

    pub fn add_hardware(&mut self, name: String) {
        if !self.hardware_types.contains(&name) {
            self.hardware_types.push(name);
        }
    }

    pub fn add_link(&mut self, from: String, to: String, latency: f64, bandwidth: f64) {
        self.links.insert(
            (from.clone(), to.clone()),
            CommunicationLink {
                latency_per_unit: latency,
                bandwidth_factor: bandwidth,
            },
        );
    }

    pub fn get_link(&self, from: &str, to: &str) -> Option<CommunicationLink> {
        self.links.get(&(from.to_string(), to.to_string())).copied()
    }

    pub fn is_connected(&self, from: &str, to: &str) -> bool {
        // Same hardware is always connected with zero cost
        if from == to {
            return true;
        }
        self.links.contains_key(&(from.to_string(), to.to_string()))
    }
}

impl Default for HardwareTopology {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Clone)]
pub struct DataflowGraph {
    pub blocks: HashMap<String, Block>,
    pub rewrites: Vec<RewriteRule>,
    pub topology: HardwareTopology,
}

impl DataflowGraph {
    pub fn new() -> Self {
        Self {
            blocks: HashMap::new(),
            rewrites: Vec::new(),
            topology: HardwareTopology::new(),
        }
    }

    pub fn print_summary(&self) {
        println!("=== Dataflow Graph Summary ===");
        
        if !self.topology.hardware_types.is_empty() {
            println!("\nHardware Types: {}", self.topology.hardware_types.len());
            for hw in &self.topology.hardware_types {
                println!("  - {}", hw);
            }

            println!("\nCommunication Links: {}", self.topology.links.len());
            for ((from, to), link) in &self.topology.links {
                println!(
                    "  {} -> {}: latency={:.2}, bandwidth={:.2}",
                    from, to, link.latency_per_unit, link.bandwidth_factor
                );
            }
        }

        println!("\nBlocks: {}", self.blocks.len());
        for (id, block) in &self.blocks {
            println!("  Block '{}':", id);
            for impl_def in &block.implementations {
                println!(
                    "    - {} ({}) with {} operations",
                    impl_def.name,
                    impl_def.hardware,
                    impl_def.ops.len()
                );
                if !impl_def.deps.is_empty() {
                    println!("      deps: {:?}", impl_def.deps);
                }
            }
        }

        if !self.rewrites.is_empty() {
            println!("\nRewrite Rules: {}", self.rewrites.len());
            for rule in &self.rewrites {
                println!("  {}", rule);
            }
        }
    }
}

impl Default for DataflowGraph {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_hardware_display() {
        assert_eq!(format!("{}", Hardware::new("CPU".to_string())), "CPU");
        assert_eq!(format!("{}", Hardware::new("GPU".to_string())), "GPU");
    }

    #[test]
    fn test_pattern_display() {
        let pattern = Pattern::Impl("blk1_cpu".to_string(), Hardware::new("CPU".to_string()));
        assert_eq!(format!("{}", pattern), "blk1_cpu@CPU");
    }

    #[test]
    fn test_dataflow_graph_creation() {
        let mut graph = DataflowGraph::new();
        assert_eq!(graph.blocks.len(), 0);
        assert_eq!(graph.rewrites.len(), 0);

        graph.blocks.insert(
            "blk1".to_string(),
            Block {
                id: "blk1".to_string(),
                implementations: vec![],
            },
        );
        assert_eq!(graph.blocks.len(), 1);
    }

    #[test]
    fn test_hardware_topology() {
        let mut topo = HardwareTopology::new();
        topo.add_hardware("CPU".to_string());
        topo.add_hardware("GPU".to_string());
        topo.add_link("CPU".to_string(), "GPU".to_string(), 10.0, 1.0);

        assert_eq!(topo.hardware_types.len(), 2);
        assert!(topo.is_connected("CPU", "GPU"));
        assert!(!topo.is_connected("GPU", "CPU")); // Directional
        assert!(topo.is_connected("CPU", "CPU")); // Same hardware
        
        let link = topo.get_link("CPU", "GPU").unwrap();
        assert_eq!(link.latency_per_unit, 10.0);
        assert_eq!(link.bandwidth_factor, 1.0);
    }
}