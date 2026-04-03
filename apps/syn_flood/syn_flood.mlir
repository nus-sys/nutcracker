!Hash5Tuple = !p4hir.extern<"Hash5Tuple">
!anon = !p4hir.enum<compute_hash, drop>
!b13i = !p4hir.bit<13>
!b16i = !p4hir.bit<16>
!b1i = !p4hir.bit<1>
!b32i = !p4hir.bit<32>
!b3i = !p4hir.bit<3>
!b4i = !p4hir.bit<4>
!b8i = !p4hir.bit<8>
!b9i = !p4hir.bit<9>
!error = !p4hir.error<NoError, PacketTooShort, NoMatch, StackOutOfBounds, HeaderTooShort, ParserTimeout, ParserInvalidArgument>
!infint = !p4hir.infint
!packet_in = !p4hir.extern<"packet_in">
!packet_out = !p4hir.extern<"packet_out">
!string = !p4hir.string
!type_I = !p4hir.type_var<"I">
!type_MH = !p4hir.type_var<"MH">
!type_MM = !p4hir.type_var<"MM">
!type_T = !p4hir.type_var<"T">
!validity_bit = !p4hir.validity.bit
#exact = #p4hir.match_kind<"exact">
#false = #p4hir.bool<false> : !p4hir.bool
#in = #p4hir<dir in>
#out = #p4hir<dir out>
#undir = #p4hir<dir undir>
!Counter_b32i = !p4hir.extern<"Counter"<!b32i>>
!ipv4_t = !p4hir.header<"ipv4_t", version: !b4i, ihl: !b4i, diffserv: !b8i, total_len: !b16i, identification: !b16i, flags: !b3i, frag_offset: !b13i, ttl: !b8i, protocol: !b8i, hdr_checksum: !b16i, src_addr: !b32i, dst_addr: !b32i, __valid: !validity_bit>
!main_metadata_t = !p4hir.struct<"main_metadata_t", hash_index: !b32i>
!nc_standard_metadata_t = !p4hir.struct<"nc_standard_metadata_t", ingress_port: !b9i, egress_port: !b9i, packet_length: !b32i, drop: !b1i>
!syn_detect = !p4hir.struct<"syn_detect", hit: !p4hir.bool, miss: !p4hir.bool, action_run: !anon>
!tcp_t = !p4hir.header<"tcp_t", src_port: !b16i, dst_port: !b16i, seq_no: !b32i, ack_no: !b32i, data_offset: !b4i, res: !b3i, ecn: !b3i, urg: !b1i, ack: !b1i, psh: !b1i, rst: !b1i, syn: !b1i, fin: !b1i, window: !b16i, checksum: !b16i, urgent_ptr: !b16i, __valid: !validity_bit>
#int-1_b1i = #p4hir.int<1> : !b1i
#int0_infint = #p4hir.int<0> : !infint
#int1024_infint = #p4hir.int<1024> : !infint
#int1_infint = #p4hir.int<1> : !infint
#int6_b8i = #p4hir.int<6> : !b8i
#valid = #p4hir<validity.bit valid> : !validity_bit
!MainDeparserT_type_MH_type_MM = !p4hir.control<"MainDeparserT"<!type_MH, !type_MM>, (!packet_out, !type_MH, !type_MM, !nc_standard_metadata_t)>
!MainParserT_type_MH_type_MM = !p4hir.parser<"MainParserT"<!type_MH, !type_MM>, (!packet_in, !p4hir.ref<!type_MH>, !p4hir.ref<!type_MM>, !nc_standard_metadata_t)>
!main_headers_t = !p4hir.struct<"main_headers_t", ipv4: !ipv4_t, tcp: !tcp_t>
!MainControlT_type_MH_type_MM = !p4hir.control<"MainControlT"<!type_MH, !type_MM>, (!p4hir.ref<!type_MH>, !p4hir.ref<!type_MM>, !p4hir.ref<!nc_standard_metadata_t>)>
!MainDeparser = !p4hir.control<"MainDeparser", (!packet_out, !main_headers_t, !main_metadata_t, !nc_standard_metadata_t)>
!NC_PIPELINE_main_headers_t_main_metadata_t = !p4hir.package<"NC_PIPELINE"<!main_headers_t, !main_metadata_t>>
!MainControl = !p4hir.control<"MainControl", (!p4hir.ref<!main_headers_t>, !p4hir.ref<!main_metadata_t>, !p4hir.ref<!nc_standard_metadata_t>)>
!MainParser = !p4hir.parser<"MainParser", (!packet_in, !p4hir.ref<!main_headers_t>, !p4hir.ref<!main_metadata_t>, !nc_standard_metadata_t)>
module {
  p4hir.extern @packet_in {
    p4hir.overload_set @extract {
      p4hir.func @extract_0<!type_T>(!p4hir.ref<!type_T> {p4hir.dir = #out, p4hir.param_name = "hdr"})
      p4hir.func @extract_1<!type_T>(!p4hir.ref<!type_T> {p4hir.dir = #out, p4hir.param_name = "variableSizeHeader"}, !b32i {p4hir.dir = #in, p4hir.param_name = "variableFieldSizeInBits"})
    }
    p4hir.func @lookahead<!type_T>() -> !type_T
    p4hir.func @advance(!b32i {p4hir.dir = #in, p4hir.param_name = "sizeInBits"})
    p4hir.func @length() -> !b32i
  }
  p4hir.extern @packet_out {
    p4hir.func @emit<!type_T>(!type_T {p4hir.dir = #in, p4hir.param_name = "hdr"})
  }
  p4hir.func @verify(!p4hir.bool {p4hir.dir = #in, p4hir.param_name = "check"}, !error {p4hir.dir = #in, p4hir.param_name = "toSignal"})
  p4hir.func action @NoAction() annotations {noWarn = "unused"} {
    p4hir.return
  }
  p4hir.overload_set @static_assert {
    p4hir.func @static_assert_0(!p4hir.bool {p4hir.dir = #undir, p4hir.param_name = "check"}, !string {p4hir.dir = #undir, p4hir.param_name = "message"}) -> !p4hir.bool
    p4hir.func @static_assert_1(!p4hir.bool {p4hir.dir = #undir, p4hir.param_name = "check"}) -> !p4hir.bool
  }
  p4hir.extern @Counter<[!type_I]> {
    p4hir.func @Counter(!b32i {p4hir.dir = #undir, p4hir.param_name = "size"})
    p4hir.func @count(!type_I {p4hir.dir = #in, p4hir.param_name = "index"})
  }
  p4hir.extern @Hash5Tuple {
    p4hir.func @Hash5Tuple()
    p4hir.func @apply(!p4hir.ref<!b32i> {p4hir.dir = #out, p4hir.param_name = "result"}, !b32i {p4hir.dir = #in, p4hir.param_name = "srcAddr"}, !b32i {p4hir.dir = #in, p4hir.param_name = "dstAddr"}, !b16i {p4hir.dir = #in, p4hir.param_name = "srcPort"}, !b16i {p4hir.dir = #in, p4hir.param_name = "dstPort"}, !b8i {p4hir.dir = #in, p4hir.param_name = "proto"})
  }
  p4hir.package @NC_PIPELINE<[!type_MH, !type_MM]>("main_parser" : !MainParserT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_parser"}, "main_control" : !MainControlT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_control"}, "main_deparser" : !MainDeparserT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_deparser"})
  p4hir.parser @MainParser(%arg0: !packet_in {p4hir.dir = #undir, p4hir.param_name = "pkt"}, %arg1: !p4hir.ref<!main_headers_t> {p4hir.dir = #out, p4hir.param_name = "hdr"}, %arg2: !p4hir.ref<!main_metadata_t> {p4hir.dir = #p4hir<dir inout>, p4hir.param_name = "meta"}, %arg3: !nc_standard_metadata_t {p4hir.dir = #in, p4hir.param_name = "standard_meta"})() {
    p4hir.state @start {
      p4hir.scope {
        %ipv4_field_ref = p4hir.struct_extract_ref %arg1["ipv4"] : <!main_headers_t>
        %hdr_out_arg = p4hir.variable ["hdr_out_arg"] : <!ipv4_t>
        p4hir.call_method @packet_in::@extract<[!ipv4_t]> (%arg0, %hdr_out_arg) : !packet_in, (!p4hir.ref<!ipv4_t>) -> ()
        %val_0 = p4hir.read %hdr_out_arg : <!ipv4_t>
        p4hir.assign %val_0, %ipv4_field_ref : <!ipv4_t>
      }
      %val = p4hir.read %arg1 : <!main_headers_t>
      %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
      %protocol = p4hir.struct_extract %ipv4["protocol"] : !ipv4_t
      p4hir.transition_select %protocol : !b8i {
        p4hir.select_case {
          %c6_b8i = p4hir.const #int6_b8i
          %set = p4hir.set (%c6_b8i) : !p4hir.set<!b8i>
          p4hir.yield %set : !p4hir.set<!b8i>
        } to @MainParser::@parse_tcp
        p4hir.select_case {
          %everything = p4hir.universal_set : !p4hir.set<!p4hir.dontcare>
          p4hir.yield %everything : !p4hir.set<!p4hir.dontcare>
        } to @MainParser::@accept
      }
    }
    p4hir.state @parse_tcp {
      p4hir.scope {
        %tcp_field_ref = p4hir.struct_extract_ref %arg1["tcp"] : <!main_headers_t>
        %hdr_out_arg = p4hir.variable ["hdr_out_arg"] : <!tcp_t>
        p4hir.call_method @packet_in::@extract<[!tcp_t]> (%arg0, %hdr_out_arg) : !packet_in, (!p4hir.ref<!tcp_t>) -> ()
        %val = p4hir.read %hdr_out_arg : <!tcp_t>
        p4hir.assign %val, %tcp_field_ref : <!tcp_t>
      }
      p4hir.transition to @MainParser::@accept
    }
    p4hir.state @accept {
      p4hir.parser_accept
    }
    p4hir.state @reject {
      p4hir.parser_reject
    }
    p4hir.transition to @MainParser::@start
  }
  p4hir.control @MainControl(%arg0: !p4hir.ref<!main_headers_t> {p4hir.dir = #p4hir<dir inout>, p4hir.param_name = "hdr"}, %arg1: !p4hir.ref<!main_metadata_t> {p4hir.dir = #p4hir<dir inout>, p4hir.param_name = "meta"}, %arg2: !p4hir.ref<!nc_standard_metadata_t> {p4hir.dir = #p4hir<dir inout>, p4hir.param_name = "standard_meta"})() {
    %hash_unit = p4hir.instantiate @Hash5Tuple() as "hash_unit" : () -> !Hash5Tuple
    %c1024 = p4hir.const #int1024_infint
    %cast = p4hir.cast(%c1024 : !infint) : !b32i
    %syn_count = p4hir.instantiate @Counter(%cast) as "syn_count" : (!b32i) -> !Counter_b32i
    p4hir.func action @compute_hash() {
      p4hir.scope {
        %hash_index_field_ref = p4hir.struct_extract_ref %arg1["hash_index"] : <!main_metadata_t>
        %result_out_arg = p4hir.variable ["result_out_arg"] : <!b32i>
        %val = p4hir.read %arg0 : <!main_headers_t>
        %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
        %src_addr = p4hir.struct_extract %ipv4["src_addr"] : !ipv4_t
        %val_0 = p4hir.read %arg0 : <!main_headers_t>
        %ipv4_1 = p4hir.struct_extract %val_0["ipv4"] : !main_headers_t
        %dst_addr = p4hir.struct_extract %ipv4_1["dst_addr"] : !ipv4_t
        %val_2 = p4hir.read %arg0 : <!main_headers_t>
        %tcp = p4hir.struct_extract %val_2["tcp"] : !main_headers_t
        %src_port = p4hir.struct_extract %tcp["src_port"] : !tcp_t
        %val_3 = p4hir.read %arg0 : <!main_headers_t>
        %tcp_4 = p4hir.struct_extract %val_3["tcp"] : !main_headers_t
        %dst_port = p4hir.struct_extract %tcp_4["dst_port"] : !tcp_t
        %val_5 = p4hir.read %arg0 : <!main_headers_t>
        %ipv4_6 = p4hir.struct_extract %val_5["ipv4"] : !main_headers_t
        %protocol = p4hir.struct_extract %ipv4_6["protocol"] : !ipv4_t
        p4hir.call_method @Hash5Tuple::@apply (%hash_unit, %result_out_arg, %src_addr, %dst_addr, %src_port, %dst_port, %protocol) : !Hash5Tuple, (!p4hir.ref<!b32i>, !b32i, !b32i, !b16i, !b16i, !b8i) -> ()
        %val_7 = p4hir.read %result_out_arg : <!b32i>
        p4hir.assign %val_7, %hash_index_field_ref : <!b32i>
      }
      p4hir.return
    }
    p4hir.func action @count_syn() {
      %val = p4hir.read %arg1 : <!main_metadata_t>
      %hash_index = p4hir.struct_extract %val["hash_index"] : !main_metadata_t
      p4hir.call_method @Counter::@count (%syn_count, %hash_index) : !Counter_b32i, (!b32i) -> ()
      p4hir.return
    }
    p4hir.func action @drop() {
      %drop_field_ref = p4hir.struct_extract_ref %arg2["drop"] : <!nc_standard_metadata_t>
      %c-1_b1i = p4hir.const #int-1_b1i
      %cast_0 = p4hir.cast(%c-1_b1i : !b1i) : !b1i
      p4hir.assign %cast_0, %drop_field_ref : <!b1i>
      p4hir.return
    }
    p4hir.table @syn_detect {
      p4hir.table_key {
        %val = p4hir.read %arg0 : <!main_headers_t>
        %tcp = p4hir.struct_extract %val["tcp"] : !main_headers_t
        %syn = p4hir.struct_extract %tcp["syn"] : !tcp_t
        p4hir.match_key #exact %syn : !b1i annotations {name = "hdr.tcp.syn"}
        %val_0 = p4hir.read %arg0 : <!main_headers_t>
        %tcp_1 = p4hir.struct_extract %val_0["tcp"] : !main_headers_t
        %ack = p4hir.struct_extract %tcp_1["ack"] : !tcp_t
        p4hir.match_key #exact %ack : !b1i annotations {name = "hdr.tcp.ack"}
        %val_2 = p4hir.read %arg0 : <!main_headers_t>
        %tcp_3 = p4hir.struct_extract %val_2["tcp"] : !main_headers_t
        %dst_port = p4hir.struct_extract %tcp_3["dst_port"] : !tcp_t
        p4hir.match_key #exact %dst_port : !b16i annotations {name = "hdr.tcp.dst_port"}
      }
      p4hir.table_actions {
        p4hir.table_action @compute_hash() {
          p4hir.call @compute_hash () : () -> ()
        }
        p4hir.table_action @drop() {
          p4hir.call @drop () : () -> ()
        }
      }
      p4hir.table_default_action {
        p4hir.call @compute_hash () : () -> ()
      }
    }
    p4hir.control_apply {
      %ipv4_field_ref = p4hir.struct_extract_ref %arg0["ipv4"] : <!main_headers_t>
      %__valid_field_ref = p4hir.struct_extract_ref %ipv4_field_ref["__valid"] : <!ipv4_t>
      %val = p4hir.read %__valid_field_ref : <!validity_bit>
      %valid = p4hir.const #valid
      %eq = p4hir.cmp(eq, %val, %valid) : !validity_bit, !p4hir.bool
      %0 = p4hir.ternary(%eq, true {
        %tcp_field_ref = p4hir.struct_extract_ref %arg0["tcp"] : <!main_headers_t>
        %__valid_field_ref_0 = p4hir.struct_extract_ref %tcp_field_ref["__valid"] : <!tcp_t>
        %val_1 = p4hir.read %__valid_field_ref_0 : <!validity_bit>
        %valid_2 = p4hir.const #valid
        %eq_3 = p4hir.cmp(eq, %val_1, %valid_2) : !validity_bit, !p4hir.bool
        p4hir.yield %eq_3 : !p4hir.bool
      }, false {
        %false = p4hir.const #false
        p4hir.yield %false : !p4hir.bool
      }) : (!p4hir.bool) -> !p4hir.bool
      p4hir.if %0 {
        %syn_detect_apply_result = p4hir.table_apply @syn_detect : !syn_detect
        %val_0 = p4hir.read %arg0 : <!main_headers_t>
        %tcp = p4hir.struct_extract %val_0["tcp"] : !main_headers_t
        %syn = p4hir.struct_extract %tcp["syn"] : !tcp_t
        %c1 = p4hir.const #int1_infint
        %cast_1 = p4hir.cast(%c1 : !infint) : !b1i
        %eq_2 = p4hir.cmp(eq, %syn, %cast_1) : !b1i, !p4hir.bool
        %1 = p4hir.ternary(%eq_2, true {
          %val_3 = p4hir.read %arg0 : <!main_headers_t>
          %tcp_4 = p4hir.struct_extract %val_3["tcp"] : !main_headers_t
          %ack = p4hir.struct_extract %tcp_4["ack"] : !tcp_t
          %c0 = p4hir.const #int0_infint
          %cast_5 = p4hir.cast(%c0 : !infint) : !b1i
          %eq_6 = p4hir.cmp(eq, %ack, %cast_5) : !b1i, !p4hir.bool
          p4hir.yield %eq_6 : !p4hir.bool
        }, false {
          %false = p4hir.const #false
          p4hir.yield %false : !p4hir.bool
        }) : (!p4hir.bool) -> !p4hir.bool
        p4hir.if %1 {
          p4hir.call @count_syn () : () -> ()
        }
      }
    }
  }
  p4hir.control @MainDeparser(%arg0: !packet_out {p4hir.dir = #undir, p4hir.param_name = "pkt"}, %arg1: !main_headers_t {p4hir.dir = #in, p4hir.param_name = "hdr"}, %arg2: !main_metadata_t {p4hir.dir = #in, p4hir.param_name = "meta"}, %arg3: !nc_standard_metadata_t {p4hir.dir = #in, p4hir.param_name = "standard_meta"})() {
    p4hir.control_apply {
      %ipv4 = p4hir.struct_extract %arg1["ipv4"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!ipv4_t]> (%arg0, %ipv4) : !packet_out, (!ipv4_t) -> ()
      %tcp = p4hir.struct_extract %arg1["tcp"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!tcp_t]> (%arg0, %tcp) : !packet_out, (!tcp_t) -> ()
    }
  }
  %MainParser = p4hir.instantiate @MainParser() as "MainParser" : () -> !MainParser
  %MainControl = p4hir.instantiate @MainControl() as "MainControl" : () -> !MainControl
  %MainDeparser = p4hir.instantiate @MainDeparser() as "MainDeparser" : () -> !MainDeparser
  %main = p4hir.instantiate @NC_PIPELINE(%MainParser, %MainControl, %MainDeparser) as "main" : (!MainParser, !MainControl, !MainDeparser) -> !NC_PIPELINE_main_headers_t_main_metadata_t
}
