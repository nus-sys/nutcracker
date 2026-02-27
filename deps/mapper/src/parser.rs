// src/parser.rs
use std::collections::HashMap;
use std::sync::Arc;
use thiserror::Error;
use ordered_float::OrderedFloat;

use crate::dataflow::*;
use egglog_ast::generic_ast::Literal;
use egglog_ast::span::{EgglogSpan, Span, SrcFile};

#[derive(Debug, Error)]
pub enum ParseError {
    #[error("Parse error at {0}: {1}")]
    Parse(Span, String),

    #[error("{0}")]
    Syntax(String),
}

macro_rules! error {
    ($span:expr, $($fmt:tt)*) => {
        Err(ParseError::Parse($span, format!($($fmt)*)))
    };
}

#[derive(Clone)]
enum Sexp {
    Literal(Literal, Span),
    Atom(String, Span),
    List(Vec<Sexp>, Span),
}

impl Sexp {
    fn span(&self) -> Span {
        match self {
            Sexp::Literal(_, span) => span.clone(),
            Sexp::Atom(_, span) => span.clone(),
            Sexp::List(_, span) => span.clone(),
        }
    }

    fn expect_atom(&self) -> Result<String, ParseError> {
        if let Sexp::Atom(symbol, _) = self {
            return Ok(symbol.clone());
        }
        error!(self.span(), "expected atom")
    }

    fn expect_list(&self) -> Result<&[Sexp], ParseError> {
        if let Sexp::List(sexps, _) = self {
            return Ok(sexps);
        }
        error!(self.span(), "expected list")
    }

    fn as_call(&self) -> Option<(&str, &[Sexp])> {
        if let Sexp::List(sexps, _) = self {
            if let [Sexp::Atom(func, _), args @ ..] = sexps.as_slice() {
                return Some((func.as_str(), args));
            }
        }
        None
    }
}

struct SexpParser {
    source: Arc<SrcFile>,
    index: usize,
}

impl SexpParser {
    fn new(name: Option<String>, contents: &str) -> Self {
        SexpParser {
            source: Arc::new(SrcFile {
                name,
                contents: contents.to_string(),
            }),
            index: 0,
        }
    }

    fn current_char(&self) -> Option<char> {
        self.source.contents[self.index..].chars().next()
    }

    fn advance_char(&mut self) {
        loop {
            self.index += 1;
            if self.index >= self.source.contents.len()
                || self.source.contents.is_char_boundary(self.index)
            {
                break;
            }
        }
    }

    fn skip_whitespace(&mut self) {
        let mut in_comment = false;
        loop {
            match self.current_char() {
                None => break,
                Some(';') => in_comment = true,
                Some('\n') => in_comment = false,
                Some(c) if c.is_whitespace() => {}
                Some(_) if in_comment => {}
                Some(_) => break,
            }
            self.advance_char();
        }
    }

    fn parse_all(&mut self) -> Result<Vec<Sexp>, ParseError> {
        let mut sexps = Vec::new();
        self.skip_whitespace();
        while self.index < self.source.contents.len() {
            sexps.push(self.parse_one()?);
            self.skip_whitespace();
        }
        Ok(sexps)
    }

    fn parse_one(&mut self) -> Result<Sexp, ParseError> {
        let mut stack: Vec<(EgglogSpan, Vec<Sexp>)> = vec![];

        loop {
            self.skip_whitespace();

            let start = self.index;
            let Some(ch) = self.current_char() else {
                if stack.is_empty() {
                    return error!(
                        Span::Egglog(Arc::new(EgglogSpan {
                            file: self.source.clone(),
                            i: start,
                            j: start,
                        })),
                        "unexpected end of input"
                    );
                }
                return error!(
                    Span::Egglog(Arc::new(EgglogSpan {
                        file: self.source.clone(),
                        i: start,
                        j: start,
                    })),
                    "unclosed parenthesis"
                );
            };

            let sexp = match ch {
                '(' => {
                    self.advance_char();
                    stack.push((
                        EgglogSpan {
                            file: self.source.clone(),
                            i: start,
                            j: self.index,
                        },
                        vec![],
                    ));
                    continue;
                }
                ')' => {
                    if stack.is_empty() {
                        return error!(
                            Span::Egglog(Arc::new(EgglogSpan {
                                file: self.source.clone(),
                                i: start,
                                j: start + 1,
                            })),
                            "unexpected )"
                        );
                    }
                    self.advance_char();
                    let (mut span, list) = stack.pop().unwrap();
                    span.j = self.index;
                    Sexp::List(list, Span::Egglog(Arc::new(span)))
                }
                '"' => {
                    self.advance_char();
                    let mut s = String::new();
                    let mut in_escape = false;

                    loop {
                        match self.current_char() {
                            None => {
                                return error!(
                                    Span::Egglog(Arc::new(EgglogSpan {
                                        file: self.source.clone(),
                                        i: start,
                                        j: self.index,
                                    })),
                                    "unterminated string"
                                )
                            }
                            Some('"') if !in_escape => {
                                self.advance_char();
                                break;
                            }
                            Some('\\') if !in_escape => in_escape = true,
                            Some(c) => {
                                s.push(match (in_escape, c) {
                                    (false, c) => c,
                                    (true, 'n') => '\n',
                                    (true, 't') => '\t',
                                    (true, '\\') => '\\',
                                    (true, '"') => '"',
                                    (true, c) => c,
                                });
                                in_escape = false;
                            }
                        }
                        self.advance_char();
                    }

                    Sexp::Literal(
                        Literal::String(s),
                        Span::Egglog(Arc::new(EgglogSpan {
                            file: self.source.clone(),
                            i: start,
                            j: self.index,
                        })),
                    )
                }
                _ => {
                    while let Some(ch) = self.current_char() {
                        if ch.is_whitespace() || ch == '(' || ch == ')' || ch == ';' {
                            break;
                        }
                        self.advance_char();
                    }

                    let span = Span::Egglog(Arc::new(EgglogSpan {
                        file: self.source.clone(),
                        i: start,
                        j: self.index,
                    }));
                    let text = span.string();

                    if text == "true" {
                        Sexp::Literal(Literal::Bool(true), span)
                    } else if text == "false" {
                        Sexp::Literal(Literal::Bool(false), span)
                    } else if let Ok(i) = text.parse::<i64>() {
                        Sexp::Literal(Literal::Int(i), span)
                    } else if let Ok(f) = text.parse::<f64>() {
                        Sexp::Literal(Literal::Float(OrderedFloat(f)), span)
                    } else {
                        Sexp::Atom(text.to_string(), span)
                    }
                }
            };

            if stack.is_empty() {
                return Ok(sexp);
            } else {
                stack.last_mut().unwrap().1.push(sexp);
            }
        }
    }
}

pub struct BlockParser {
    defined_ops: HashMap<String, Sexp>,
    pub op_costs: HashMap<String, f64>,
    pub op_types: HashMap<String, crate::cost_model::OpType>,
    pub op_sigmas: HashMap<String, f64>,
    pub hw_configs: HashMap<String, crate::cost_model::HardwareConfig>,
}

impl BlockParser {
    pub fn new() -> Self {
        Self {
            defined_ops: HashMap::new(),
            op_costs: HashMap::new(),
            op_types: HashMap::new(),
            op_sigmas: HashMap::new(),
            hw_configs: HashMap::new(),
        }
    }

    pub fn parse_file(
        &mut self,
        filename: Option<String>,
        input: &str,
    ) -> Result<DataflowGraph, ParseError> {
        let mut parser = SexpParser::new(filename, input);
        let sexps = parser.parse_all()?;
        self.extract_dataflow_graph(&sexps)
    }

    fn extract_dataflow_graph(&mut self, sexps: &[Sexp]) -> Result<DataflowGraph, ParseError> {
        let mut blocks_map: HashMap<String, Vec<BlockImpl>> = HashMap::new();
        let mut rewrites = Vec::new();
        let mut topology = HardwareTopology::new();

        for sexp in sexps {
            if let Some((cmd, args)) = sexp.as_call() {
                match cmd {
                    "let" => {
                        if args.len() == 2 {
                            let var = args[0].expect_atom()?;
                            self.defined_ops.insert(var, args[1].clone());
                        }
                    }
                    "function" => {
                        if !args.is_empty() {
                            let func_name = args[0].expect_atom()?;
                            let cost = self.extract_cost_from_function(args)?;
                            self.op_costs.insert(func_name, cost);
                        }
                    }
                    "hardware" => {
                        // (hardware CPU)
                        // (hardware RX) - now RX is just another hardware type
                        if !args.is_empty() {
                            let hw_name = args[0].expect_atom()?;
                            topology.add_hardware(hw_name);
                        }
                    }
                    "link" => {
                        // (link RX DRMT 0.0 1.0) - works just like any other link!
                        // (link RX DPA 20.0 0.5)
                        // (link CPU GPU 10.0 1.0)
                        if args.len() >= 3 {
                            let from = args[0].expect_atom()?;
                            let to = args[1].expect_atom()?;
                            let latency = self.extract_float(&args[2])?;
                            let bandwidth = if args.len() >= 4 {
                                self.extract_float(&args[3])?
                            } else {
                                1.0 // Default bandwidth
                            };
                            topology.add_link(from, to, latency, bandwidth);
                        }
                    }
                    "implement" | "Impt" => {
                        let impl_def = self.parse_implementation(args)?;
                        let block_id = self.extract_block_id(&impl_def.name);
                        blocks_map
                            .entry(block_id.to_string())
                            .or_insert_with(Vec::new)
                            .push(impl_def);
                    }
                    "rewrite" => {
                        if args.len() >= 2 {
                            let lhs = self.sexp_to_pattern(&args[0])?;
                            let rhs = self.sexp_to_pattern(&args[1])?;
                            rewrites.push(RewriteRule { lhs, rhs });
                        }
                    }
                    _ => {}
                }
            }
        }

        let mut blocks = HashMap::new();
        for (block_id, impls) in blocks_map {
            blocks.insert(
                block_id.clone(),
                Block {
                    id: block_id,
                    implementations: impls,
                },
            );
        }

        Ok(DataflowGraph { blocks, rewrites, topology })
    }

    fn extract_keyword_int(&self, args: &[Sexp], keyword: &str) -> Result<Option<i64>, ParseError> {
        for i in 0..args.len() {
            if let Sexp::Atom(kw, _) = &args[i] {
                if kw == keyword && i + 1 < args.len() {
                    if let Sexp::Literal(Literal::Int(val), _) = &args[i + 1] {
                        return Ok(Some(*val));
                    }
                }
            }
        }
        Ok(None)
    }

    fn extract_keyword_float(&self, args: &[Sexp], keyword: &str) -> Result<Option<f64>, ParseError> {
        for i in 0..args.len() {
            if let Sexp::Atom(kw, _) = &args[i] {
                if kw == keyword && i + 1 < args.len() {
                    return Ok(Some(self.extract_float(&args[i + 1])?));
                }
            }
        }
        Ok(None)
    }

    fn extract_cost_from_function(&self, args: &[Sexp]) -> Result<f64, ParseError> {
        for i in 0..args.len() {
            if let Sexp::Atom(keyword, _) = &args[i] {
                if keyword == ":cost" && i + 1 < args.len() {
                    if let Sexp::Literal(Literal::Int(cost), _) = &args[i + 1] {
                        return Ok(*cost as f64);
                    }
                    if let Sexp::Literal(Literal::Float(cost), _) = &args[i + 1] {
                        return Ok(cost.0);
                    }
                }
            }
        }
        Ok(1.0)
    }

    fn extract_op_type_from_function(&self, args: &[Sexp]) -> Result<crate::cost_model::OpType, ParseError> {
        for i in 0..args.len() {
            if let Sexp::Atom(keyword, _) = &args[i] {
                if keyword == ":type" && i + 1 < args.len() {
                    if let Sexp::Atom(type_str, _) = &args[i + 1] {
                        match type_str.as_str() {
                            "stateless" => return Ok(crate::cost_model::OpType::Stateless),
                            "stateful" => return Ok(crate::cost_model::OpType::Stateful),
                            _ => return Err(ParseError::Syntax(format!(
                                "Unknown op type: {}. Expected 'stateless' or 'stateful'",
                                type_str
                            ))),
                        }
                    }
                }
            }
        }
        Ok(crate::cost_model::OpType::Stateless) // Default
    }

    fn extract_sigma_from_function(&self, args: &[Sexp]) -> Result<f64, ParseError> {
        for i in 0..args.len() {
            if let Sexp::Atom(keyword, _) = &args[i] {
                if keyword == ":sigma" && i + 1 < args.len() {
                    return self.extract_float(&args[i + 1]);
                }
            }
        }
        Ok(0.0) // Default
    }

    fn extract_float(&self, sexp: &Sexp) -> Result<f64, ParseError> {
        match sexp {
            Sexp::Literal(Literal::Int(i), _) => Ok(*i as f64),
            Sexp::Literal(Literal::Float(f), _) => Ok(f.0),
            _ => Err(ParseError::Syntax("Expected number".to_string())),
        }
    }

    fn parse_implementation(&self, args: &[Sexp]) -> Result<BlockImpl, ParseError> {
        if args.len() != 4 {
            return Err(ParseError::Syntax(format!(
                "implement requires 4 arguments, got {}",
                args.len()
            )));
        }

        let name = args[0].expect_atom()?;
        let hardware = self.parse_hardware(&args[1])?;
        let deps = self.parse_deps(&args[2])?;
        let ops = self.parse_ops(&args[3])?;

        Ok(BlockImpl {
            name,
            hardware,
            deps,
            ops,
        })
    }

    fn parse_hardware(&self, sexp: &Sexp) -> Result<Hardware, ParseError> {
        let hw_str = sexp.expect_atom()?;
        Ok(Hardware::new(hw_str))
    }

    fn parse_deps(&self, sexp: &Sexp) -> Result<Vec<String>, ParseError> {
        if let Sexp::Literal(Literal::Unit, _) = sexp {
            return Ok(Vec::new());
        }
        if let Ok(list) = sexp.expect_list() {
            return list.iter().map(|s| s.expect_atom()).collect();
        }
        Ok(Vec::new())
    }

    fn parse_ops(&self, sexp: &Sexp) -> Result<Vec<Operation>, ParseError> {
        // Resolve if it's a variable
        let resolved_sexp = if let Sexp::Atom(name, _) = sexp {
            self.defined_ops
                .get(name)
                .ok_or_else(|| ParseError::Syntax(format!("Undefined variable: {}", name)))?
        } else {
            sexp
        };

        let (func, args) = resolved_sexp
            .as_call()
            .ok_or_else(|| ParseError::Syntax("Expected (vec-of ...)".to_string()))?;

        if func != "vec-of" {
            return Err(ParseError::Syntax(format!(
                "Expected vec-of, found {}",
                func
            )));
        }

        let mut operations = Vec::new();
        for arg in args {
            let op_name = arg.expect_atom()?;

            let op_sexp = self.defined_ops.get(&op_name).ok_or_else(|| {
                ParseError::Syntax(format!("Undefined operation: {}", op_name))
            })?;

            operations.push(Operation {
                var_name: op_name,
                expr: self.sexp_to_op_expr(op_sexp)?,
            });
        }

        Ok(operations)
    }

    fn sexp_to_op_expr(&self, sexp: &Sexp) -> Result<OpExpr, ParseError> {
        match sexp {
            Sexp::Atom(name, _) => Ok(OpExpr::Var(name.clone())),
            Sexp::Literal(lit, _) => Ok(OpExpr::Literal(lit.clone())),
            Sexp::List(_, _) => {
                let (func, args) = sexp
                    .as_call()
                    .ok_or_else(|| ParseError::Syntax("Expected call".to_string()))?;

                let op_args = args
                    .iter()
                    .map(|arg| self.sexp_to_op_arg(arg))
                    .collect::<Result<Vec<_>, _>>()?;

                Ok(OpExpr::Call {
                    func: func.to_string(),
                    args: op_args,
                })
            }
        }
    }

    fn sexp_to_op_arg(&self, sexp: &Sexp) -> Result<OpArg, ParseError> {
        match sexp {
            Sexp::Atom(name, _) => Ok(OpArg::Var(name.clone())),
            Sexp::Literal(lit, _) => Ok(OpArg::Literal(lit.clone())),
            Sexp::List(_, _) => {
                let op_expr = self.sexp_to_op_expr(sexp)?;
                Ok(OpArg::Nested(Box::new(op_expr)))
            }
        }
    }

    fn sexp_to_pattern(&self, sexp: &Sexp) -> Result<Pattern, ParseError> {
        match sexp {
            Sexp::Atom(name, _) => {
                if name.starts_with('?') {
                    return Ok(Pattern::Var(name[1..].to_string()));
                }

                let hw = self.infer_hw_from_impl_name(name);
                Ok(Pattern::Impl(name.clone(), hw))
            }
            Sexp::List(_, _) => {
                if let Some((func, _args)) = sexp.as_call() {
                    let hw = self.infer_hw_from_impl_name(func);
                    Ok(Pattern::Impl(func.to_string(), hw))
                } else {
                    Err(ParseError::Syntax("Invalid pattern".to_string()))
                }
            }
            Sexp::Literal(_, _) => Err(ParseError::Syntax(
                "Literals not allowed in patterns".to_string(),
            )),
        }
    }

    fn infer_hw_from_impl_name(&self, impl_name: &str) -> Hardware {
        // Try to extract hardware from implementation name (e.g., "blk1_cpu" -> "CPU")
        if let Some(underscore_pos) = impl_name.rfind('_') {
            let hw_part = &impl_name[underscore_pos + 1..];
            Hardware::new(hw_part.to_uppercase())
        } else {
            Hardware::new(String::new())
        }
    }

    fn extract_block_id<'a>(&self, impl_name: &'a str) -> &'a str {
        impl_name.split('_').next().unwrap_or(impl_name)
    }
}

impl Default for BlockParser {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_simple_block() {
        let input = r#"
            (let add (Add a b))
            (implement block1_cpu CPU () (vec-of add))
        "#;

        let mut parser = BlockParser::new();
        let result = parser.parse_file(None, input).unwrap();

        assert_eq!(result.blocks.len(), 1);
        assert!(result.blocks.contains_key("block1"));
    }

    #[test]
    fn test_parse_function_with_cost() {
        let input = r#"
            (function test_op (Type) Type :cost 15)
        "#;

        let mut parser = BlockParser::new();
        let _ = parser.parse_file(None, input).unwrap();

        assert_eq!(parser.op_costs.get("test_op"), Some(&15.0));
    }

    #[test]
    fn test_parse_rewrite() {
        let input = r#"
            (let add (Add a b))
            (implement blk1_cpu CPU () (vec-of add))
            (implement blk1_drmt DRMT () (vec-of add))
            (rewrite blk1_cpu blk1_drmt)
        "#;

        let mut parser = BlockParser::new();
        let result = parser.parse_file(None, input).unwrap();

        assert_eq!(result.rewrites.len(), 1);
    }

    #[test]
    fn test_parse_hardware_topology() {
        let input = r#"
            (hardware CPU)
            (hardware GPU)
            (link CPU GPU 10.0 1.0)
        "#;

        let mut parser = BlockParser::new();
        let result = parser.parse_file(None, input).unwrap();

        assert_eq!(result.topology.hardware_types.len(), 2);
        assert!(result.topology.is_connected("CPU", "GPU"));
        
        let link = result.topology.get_link("CPU", "GPU").unwrap();
        assert_eq!(link.latency_per_unit, 10.0);
        assert_eq!(link.bandwidth_factor, 1.0);
    }
}