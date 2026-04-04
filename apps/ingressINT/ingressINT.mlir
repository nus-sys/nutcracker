!anon = !p4hir.enum<ipv4_forward, drop, NoAction>
!anon1 = !p4hir.enum<enable_int_transit, NoAction>
!b13i = !p4hir.bit<13>
!b16i = !p4hir.bit<16>
!b1i = !p4hir.bit<1>
!b2i = !p4hir.bit<2>
!b32i = !p4hir.bit<32>
!b3i = !p4hir.bit<3>
!b48i = !p4hir.bit<48>
!b4i = !p4hir.bit<4>
!b5i = !p4hir.bit<5>
!b6i = !p4hir.bit<6>
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
#in = #p4hir<dir in>
#out = #p4hir<dir out>
#undir = #p4hir<dir undir>
!Register_b16i = !p4hir.extern<"Register"<!b16i>>
!ethernet_t = !p4hir.header<"ethernet_t", dstAddr: !b48i, srcAddr: !b48i, etherType: !b16i, __valid: !validity_bit>
!int_config_transit = !p4hir.struct<"int_config_transit", hit: !p4hir.bool, miss: !p4hir.bool, action_run: !anon1>
!int_header_t = !p4hir.header<"int_header_t", version: !b4i, rep: !b2i, c: !b1i, e: !b1i, rsvd1: !b5i, ins_cnt: !b5i, max_hop_cnt: !b8i, total_hop_cnt: !b8i, instruction_mask_0003: !b4i, instruction_mask_0407: !b4i, instruction_mask_0811: !b4i, instruction_mask_1215: !b4i, rsvd2: !b16i, __valid: !validity_bit>
!int_switch_id_t = !p4hir.header<"int_switch_id_t", switch_id: !b32i, __valid: !validity_bit>
!ipv4_lpm = !p4hir.struct<"ipv4_lpm", hit: !p4hir.bool, miss: !p4hir.bool, action_run: !anon>
!ipv4_t = !p4hir.header<"ipv4_t", version: !b4i, ihl: !b4i, dscp: !b6i, ecn: !b2i, totalLen: !b16i, identification: !b16i, flags: !b3i, fragOffset: !b13i, ttl: !b8i, protocol: !b8i, hdrChecksum: !b16i, srcAddr: !b32i, dstAddr: !b32i, __valid: !validity_bit>
!main_metadata_t = !p4hir.struct<"main_metadata_t", int_node_type: !b3i, switch_id: !b32i, int_enabled: !b1i, bloom_hash: !b32i, bloom_index: !b32i, bloom_counter: !b16i, egress_port: !b9i, drop_packet: !b1i>
!nc_standard_metadata_t = !p4hir.struct<"nc_standard_metadata_t", ingress_port: !b9i, egress_port: !b9i, packet_length: !b32i, drop: !b1i>
!tcp_t = !p4hir.header<"tcp_t", srcPort: !b16i, dstPort: !b16i, seqNo: !b32i, ackNo: !b32i, dataOffset: !b4i, res: !b4i, flags: !b8i, window: !b16i, checksum: !b16i, urgentPtr: !b16i, __valid: !validity_bit>
#int-1_b1i = #p4hir.int<1> : !b1i
#int-4_b3i = #p4hir.int<4> : !b3i
#int-8_b4i = #p4hir.int<8> : !b4i
#int0_b16i = #p4hir.int<0> : !b16i
#int0_b1i = #p4hir.int<0> : !b1i
#int0_b2i = #p4hir.int<0> : !b2i
#int0_b3i = #p4hir.int<0> : !b3i
#int0_b4i = #p4hir.int<0> : !b4i
#int0_b5i = #p4hir.int<0> : !b5i
#int0_infint = #p4hir.int<0> : !infint
#int100_b16i = #p4hir.int<100> : !b16i
#int12_b16i = #p4hir.int<12> : !b16i
#int1_b16i = #p4hir.int<1> : !b16i
#int1_b4i = #p4hir.int<1> : !b4i
#int1_b5i = #p4hir.int<1> : !b5i
#int1_b8i = #p4hir.int<1> : !b8i
#int1_infint = #p4hir.int<1> : !infint
#int2048_b16i = #p4hir.int<2048> : !b16i
#int20_b16i = #p4hir.int<20> : !b16i
#int256_infint = #p4hir.int<256> : !infint
#int4095_b32i = #p4hir.int<4095> : !b32i
#int4096_b32i = #p4hir.int<4096> : !b32i
#int64_infint = #p4hir.int<64> : !infint
#int6_b8i = #p4hir.int<6> : !b8i
#int73_b32i = #p4hir.int<73> : !b32i
#int8_b8i = #p4hir.int<8> : !b8i
#valid = #p4hir<validity.bit valid> : !validity_bit
!MainDeparserT_type_MH_type_MM = !p4hir.control<"MainDeparserT"<!type_MH, !type_MM>, (!packet_out, !type_MH, !type_MM, !nc_standard_metadata_t)>
!MainParserT_type_MH_type_MM = !p4hir.parser<"MainParserT"<!type_MH, !type_MM>, (!packet_in, !p4hir.ref<!type_MH>, !p4hir.ref<!type_MM>, !nc_standard_metadata_t)>
!main_headers_t = !p4hir.struct<"main_headers_t", ethernet: !ethernet_t, ipv4: !ipv4_t, tcp: !tcp_t, int_header: !int_header_t, int_switch_id: !int_switch_id_t>
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
  p4hir.extern @meter {
    p4hir.func @meter(!b32i {p4hir.dir = #undir, p4hir.param_name = "size"}, !b8i {p4hir.dir = #undir, p4hir.param_name = "meter_type"})
    p4hir.func @execute_meter<!type_T>(!b32i {p4hir.dir = #in, p4hir.param_name = "index"}, !p4hir.ref<!type_T> {p4hir.dir = #out, p4hir.param_name = "result"})
  }
  p4hir.extern @Register<[!type_T]> {
    p4hir.func @Register(!b32i {p4hir.dir = #undir, p4hir.param_name = "size"})
    p4hir.func @read(!b32i {p4hir.dir = #in, p4hir.param_name = "index"}) -> !type_T
    p4hir.func @write(!b32i {p4hir.dir = #in, p4hir.param_name = "index"}, !type_T {p4hir.dir = #in, p4hir.param_name = "value"})
  }
  p4hir.extern @Hash5Tuple {
    p4hir.func @Hash5Tuple()
    p4hir.func @apply(!p4hir.ref<!b32i> {p4hir.dir = #out, p4hir.param_name = "result"}, !b32i {p4hir.dir = #in, p4hir.param_name = "srcAddr"}, !b32i {p4hir.dir = #in, p4hir.param_name = "dstAddr"}, !b16i {p4hir.dir = #in, p4hir.param_name = "srcPort"}, !b16i {p4hir.dir = #in, p4hir.param_name = "dstPort"}, !b8i {p4hir.dir = #in, p4hir.param_name = "proto"})
  }
  p4hir.package @NC_PIPELINE<[!type_MH, !type_MM]>("main_parser" : !MainParserT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_parser"}, "main_control" : !MainControlT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_control"}, "main_deparser" : !MainDeparserT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_deparser"})
  %INT_NODE_NONE = p4hir.const ["INT_NODE_NONE"] #int0_b3i
  %INT_NODE_TRANSIT = p4hir.const ["INT_NODE_TRANSIT"] #int-4_b3i
  %BLOOM_FILTER_ENTRIES = p4hir.const ["BLOOM_FILTER_ENTRIES"] #int4096_b32i
  %BLOOM_FILTER_INDEX_BITS = p4hir.const ["BLOOM_FILTER_INDEX_BITS"] #int12_b16i
  %REPORT_FREQ = p4hir.const ["REPORT_FREQ"] #int100_b16i
  %MULT_HASH_A = p4hir.const ["MULT_HASH_A"] #int73_b32i
  p4hir.parser @MainParser(%arg0: !packet_in {p4hir.dir = #undir, p4hir.param_name = "pkt"}, %arg1: !p4hir.ref<!main_headers_t> {p4hir.dir = #out, p4hir.param_name = "hdr"}, %arg2: !p4hir.ref<!main_metadata_t> {p4hir.dir = #p4hir<dir inout>, p4hir.param_name = "meta"}, %arg3: !nc_standard_metadata_t {p4hir.dir = #in, p4hir.param_name = "standard_meta"})() {
    p4hir.state @start {
      p4hir.scope {
        %ethernet_field_ref = p4hir.struct_extract_ref %arg1["ethernet"] : <!main_headers_t>
        %hdr_out_arg = p4hir.variable ["hdr_out_arg"] : <!ethernet_t>
        p4hir.call_method @packet_in::@extract<[!ethernet_t]> (%arg0, %hdr_out_arg) : !packet_in, (!p4hir.ref<!ethernet_t>) -> ()
        %val_0 = p4hir.read %hdr_out_arg : <!ethernet_t>
        p4hir.assign %val_0, %ethernet_field_ref : <!ethernet_t>
      }
      %val = p4hir.read %arg1 : <!main_headers_t>
      %ethernet = p4hir.struct_extract %val["ethernet"] : !main_headers_t
      %etherType = p4hir.struct_extract %ethernet["etherType"] : !ethernet_t
      p4hir.transition_select %etherType : !b16i {
        p4hir.select_case {
          %c2048_b16i = p4hir.const #int2048_b16i
          %set = p4hir.set (%c2048_b16i) : !p4hir.set<!b16i>
          p4hir.yield %set : !p4hir.set<!b16i>
        } to @MainParser::@parse_ipv4
        p4hir.select_case {
          %everything = p4hir.universal_set : !p4hir.set<!p4hir.dontcare>
          p4hir.yield %everything : !p4hir.set<!p4hir.dontcare>
        } to @MainParser::@accept
      }
    }
    p4hir.state @parse_ipv4 {
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
    %c4096_b32i = p4hir.const #int4096_b32i
    %bloom_filter = p4hir.instantiate @Register(%c4096_b32i) as "bloom_filter" : (!b32i) -> !Register_b16i
    p4hir.func action @compute_bloom_hash() {
      %val = p4hir.read %arg0 : <!main_headers_t>
      %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
      %srcAddr = p4hir.struct_extract %ipv4["srcAddr"] : !ipv4_t
      %src_ip = p4hir.variable ["src_ip", init] : <!b32i>
      p4hir.assign %srcAddr, %src_ip : <!b32i>
      %c73_b32i = p4hir.const #int73_b32i
      %val_0 = p4hir.read %src_ip : <!b32i>
      %mul = p4hir.binop(mul, %val_0, %c73_b32i) : !b32i
      %c20_b16i = p4hir.const #int20_b16i
      %shr = p4hir.shr(%mul, %c20_b16i : !b16i) : !b32i
      %hash_val = p4hir.variable ["hash_val", init] : <!b32i>
      p4hir.assign %shr, %hash_val : <!b32i>
      %bloom_index_field_ref = p4hir.struct_extract_ref %arg1["bloom_index"] : <!main_metadata_t>
      %c4095_b32i = p4hir.const #int4095_b32i
      %val_1 = p4hir.read %hash_val : <!b32i>
      %and = p4hir.binop(and, %val_1, %c4095_b32i) : !b32i
      p4hir.assign %and, %bloom_index_field_ref : <!b32i>
      p4hir.return
    }
    p4hir.func action @read_bloom_counter() {
      %bloom_counter_field_ref = p4hir.struct_extract_ref %arg1["bloom_counter"] : <!main_metadata_t>
      %val = p4hir.read %arg1 : <!main_metadata_t>
      %bloom_index = p4hir.struct_extract %val["bloom_index"] : !main_metadata_t
      %0 = p4hir.call_method @Register::@read (%bloom_filter, %bloom_index) : !Register_b16i, (!b32i) -> !b16i
      p4hir.assign %0, %bloom_counter_field_ref : <!b16i>
      p4hir.return
    }
    p4hir.func action @increment_bloom_counter() {
      %val = p4hir.read %arg1 : <!main_metadata_t>
      %bloom_counter = p4hir.struct_extract %val["bloom_counter"] : !main_metadata_t
      %c1_b16i = p4hir.const #int1_b16i
      %add = p4hir.binop(add, %bloom_counter, %c1_b16i) : !b16i
      %new_val = p4hir.variable ["new_val", init] : <!b16i>
      p4hir.assign %add, %new_val : <!b16i>
      %val_0 = p4hir.read %arg1 : <!main_metadata_t>
      %bloom_index = p4hir.struct_extract %val_0["bloom_index"] : !main_metadata_t
      %val_1 = p4hir.read %new_val : <!b16i>
      p4hir.call_method @Register::@write (%bloom_filter, %bloom_index, %val_1) : !Register_b16i, (!b32i, !b16i) -> ()
      p4hir.return
    }
    p4hir.func action @reset_bloom_counter() {
      %val = p4hir.read %arg1 : <!main_metadata_t>
      %bloom_index = p4hir.struct_extract %val["bloom_index"] : !main_metadata_t
      %c0 = p4hir.const #int0_infint
      %cast = p4hir.cast(%c0 : !infint) : !b16i
      p4hir.call_method @Register::@write (%bloom_filter, %bloom_index, %cast) : !Register_b16i, (!b32i, !b16i) -> ()
      p4hir.return
    }
    p4hir.func action @enable_int_transit(%arg3: !b32i {p4hir.dir = #undir, p4hir.param_name = "switch_id"}) {
      %int_node_type_field_ref = p4hir.struct_extract_ref %arg1["int_node_type"] : <!main_metadata_t>
      %c-4_b3i = p4hir.const #int-4_b3i
      p4hir.assign %c-4_b3i, %int_node_type_field_ref : <!b3i>
      %switch_id_field_ref = p4hir.struct_extract_ref %arg1["switch_id"] : <!main_metadata_t>
      p4hir.assign %arg3, %switch_id_field_ref : <!b32i>
      %int_enabled_field_ref = p4hir.struct_extract_ref %arg1["int_enabled"] : <!main_metadata_t>
      %c-1_b1i = p4hir.const #int-1_b1i
      %cast = p4hir.cast(%c-1_b1i : !b1i) : !b1i
      p4hir.assign %cast, %int_enabled_field_ref : <!b1i>
      p4hir.return
    }
    p4hir.func action @add_int_header() {
      %int_header_field_ref = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %valid = p4hir.const #valid
      %__valid_field_ref = p4hir.struct_extract_ref %int_header_field_ref["__valid"] : <!int_header_t>
      p4hir.assign %valid, %__valid_field_ref : <!validity_bit>
      %int_header_field_ref_0 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %version_field_ref = p4hir.struct_extract_ref %int_header_field_ref_0["version"] : <!int_header_t>
      %c1_b4i = p4hir.const #int1_b4i
      %cast = p4hir.cast(%c1_b4i : !b4i) : !b4i
      p4hir.assign %cast, %version_field_ref : <!b4i>
      %int_header_field_ref_1 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %rep_field_ref = p4hir.struct_extract_ref %int_header_field_ref_1["rep"] : <!int_header_t>
      %c0_b2i = p4hir.const #int0_b2i
      %cast_2 = p4hir.cast(%c0_b2i : !b2i) : !b2i
      p4hir.assign %cast_2, %rep_field_ref : <!b2i>
      %int_header_field_ref_3 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %c_field_ref = p4hir.struct_extract_ref %int_header_field_ref_3["c"] : <!int_header_t>
      %c0_b1i = p4hir.const #int0_b1i
      %cast_4 = p4hir.cast(%c0_b1i : !b1i) : !b1i
      p4hir.assign %cast_4, %c_field_ref : <!b1i>
      %int_header_field_ref_5 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %e_field_ref = p4hir.struct_extract_ref %int_header_field_ref_5["e"] : <!int_header_t>
      %c0_b1i_6 = p4hir.const #int0_b1i
      %cast_7 = p4hir.cast(%c0_b1i_6 : !b1i) : !b1i
      p4hir.assign %cast_7, %e_field_ref : <!b1i>
      %int_header_field_ref_8 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %rsvd1_field_ref = p4hir.struct_extract_ref %int_header_field_ref_8["rsvd1"] : <!int_header_t>
      %c0_b5i = p4hir.const #int0_b5i
      %cast_9 = p4hir.cast(%c0_b5i : !b5i) : !b5i
      p4hir.assign %cast_9, %rsvd1_field_ref : <!b5i>
      %int_header_field_ref_10 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %ins_cnt_field_ref = p4hir.struct_extract_ref %int_header_field_ref_10["ins_cnt"] : <!int_header_t>
      %c1_b5i = p4hir.const #int1_b5i
      %cast_11 = p4hir.cast(%c1_b5i : !b5i) : !b5i
      p4hir.assign %cast_11, %ins_cnt_field_ref : <!b5i>
      %int_header_field_ref_12 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %max_hop_cnt_field_ref = p4hir.struct_extract_ref %int_header_field_ref_12["max_hop_cnt"] : <!int_header_t>
      %c8_b8i = p4hir.const #int8_b8i
      %cast_13 = p4hir.cast(%c8_b8i : !b8i) : !b8i
      p4hir.assign %cast_13, %max_hop_cnt_field_ref : <!b8i>
      %int_header_field_ref_14 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %total_hop_cnt_field_ref = p4hir.struct_extract_ref %int_header_field_ref_14["total_hop_cnt"] : <!int_header_t>
      %c1_b8i = p4hir.const #int1_b8i
      %cast_15 = p4hir.cast(%c1_b8i : !b8i) : !b8i
      p4hir.assign %cast_15, %total_hop_cnt_field_ref : <!b8i>
      %int_header_field_ref_16 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %instruction_mask_0003_field_ref = p4hir.struct_extract_ref %int_header_field_ref_16["instruction_mask_0003"] : <!int_header_t>
      %c-8_b4i = p4hir.const #int-8_b4i
      %cast_17 = p4hir.cast(%c-8_b4i : !b4i) : !b4i
      p4hir.assign %cast_17, %instruction_mask_0003_field_ref : <!b4i>
      %int_header_field_ref_18 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %instruction_mask_0407_field_ref = p4hir.struct_extract_ref %int_header_field_ref_18["instruction_mask_0407"] : <!int_header_t>
      %c0_b4i = p4hir.const #int0_b4i
      %cast_19 = p4hir.cast(%c0_b4i : !b4i) : !b4i
      p4hir.assign %cast_19, %instruction_mask_0407_field_ref : <!b4i>
      %int_header_field_ref_20 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %instruction_mask_0811_field_ref = p4hir.struct_extract_ref %int_header_field_ref_20["instruction_mask_0811"] : <!int_header_t>
      %c0_b4i_21 = p4hir.const #int0_b4i
      %cast_22 = p4hir.cast(%c0_b4i_21 : !b4i) : !b4i
      p4hir.assign %cast_22, %instruction_mask_0811_field_ref : <!b4i>
      %int_header_field_ref_23 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %instruction_mask_1215_field_ref = p4hir.struct_extract_ref %int_header_field_ref_23["instruction_mask_1215"] : <!int_header_t>
      %c0_b4i_24 = p4hir.const #int0_b4i
      %cast_25 = p4hir.cast(%c0_b4i_24 : !b4i) : !b4i
      p4hir.assign %cast_25, %instruction_mask_1215_field_ref : <!b4i>
      %int_header_field_ref_26 = p4hir.struct_extract_ref %arg0["int_header"] : <!main_headers_t>
      %rsvd2_field_ref = p4hir.struct_extract_ref %int_header_field_ref_26["rsvd2"] : <!int_header_t>
      %c0_b16i = p4hir.const #int0_b16i
      %cast_27 = p4hir.cast(%c0_b16i : !b16i) : !b16i
      p4hir.assign %cast_27, %rsvd2_field_ref : <!b16i>
      %int_switch_id_field_ref = p4hir.struct_extract_ref %arg0["int_switch_id"] : <!main_headers_t>
      %valid_28 = p4hir.const #valid
      %__valid_field_ref_29 = p4hir.struct_extract_ref %int_switch_id_field_ref["__valid"] : <!int_switch_id_t>
      p4hir.assign %valid_28, %__valid_field_ref_29 : <!validity_bit>
      %int_switch_id_field_ref_30 = p4hir.struct_extract_ref %arg0["int_switch_id"] : <!main_headers_t>
      %switch_id_field_ref = p4hir.struct_extract_ref %int_switch_id_field_ref_30["switch_id"] : <!int_switch_id_t>
      %val = p4hir.read %arg1 : <!main_metadata_t>
      %switch_id = p4hir.struct_extract %val["switch_id"] : !main_metadata_t
      p4hir.assign %switch_id, %switch_id_field_ref : <!b32i>
      p4hir.return
    }
    p4hir.func action @ipv4_forward(%arg3: !b48i {p4hir.dir = #undir, p4hir.param_name = "src_mac"}, %arg4: !b48i {p4hir.dir = #undir, p4hir.param_name = "dst_mac"}, %arg5: !b9i {p4hir.dir = #undir, p4hir.param_name = "port"}) {
      %ethernet_field_ref = p4hir.struct_extract_ref %arg0["ethernet"] : <!main_headers_t>
      %srcAddr_field_ref = p4hir.struct_extract_ref %ethernet_field_ref["srcAddr"] : <!ethernet_t>
      p4hir.assign %arg3, %srcAddr_field_ref : <!b48i>
      %ethernet_field_ref_0 = p4hir.struct_extract_ref %arg0["ethernet"] : <!main_headers_t>
      %dstAddr_field_ref = p4hir.struct_extract_ref %ethernet_field_ref_0["dstAddr"] : <!ethernet_t>
      p4hir.assign %arg4, %dstAddr_field_ref : <!b48i>
      %ipv4_field_ref = p4hir.struct_extract_ref %arg0["ipv4"] : <!main_headers_t>
      %ttl_field_ref = p4hir.struct_extract_ref %ipv4_field_ref["ttl"] : <!ipv4_t>
      %val = p4hir.read %arg0 : <!main_headers_t>
      %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
      %ttl = p4hir.struct_extract %ipv4["ttl"] : !ipv4_t
      %c1_b8i = p4hir.const #int1_b8i
      %sub = p4hir.binop(sub, %ttl, %c1_b8i) : !b8i
      p4hir.assign %sub, %ttl_field_ref : <!b8i>
      %egress_port_field_ref = p4hir.struct_extract_ref %arg2["egress_port"] : <!nc_standard_metadata_t>
      p4hir.assign %arg5, %egress_port_field_ref : <!b9i>
      p4hir.return
    }
    p4hir.func action @drop() {
      %drop_field_ref = p4hir.struct_extract_ref %arg2["drop"] : <!nc_standard_metadata_t>
      %c-1_b1i = p4hir.const #int-1_b1i
      %cast = p4hir.cast(%c-1_b1i : !b1i) : !b1i
      p4hir.assign %cast, %drop_field_ref : <!b1i>
      p4hir.return
    }
    p4hir.table @ipv4_lpm {
      p4hir.table_key {
        %val = p4hir.read %arg0 : <!main_headers_t>
        %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
        %dstAddr = p4hir.struct_extract %ipv4["dstAddr"] : !ipv4_t
        p4hir.match_key #exact %dstAddr : !b32i annotations {name = "hdr.ipv4.dstAddr"}
      }
      p4hir.table_actions {
        p4hir.table_action @ipv4_forward(%arg3: !b48i {p4hir.param_name = "src_mac"}, %arg4: !b48i {p4hir.param_name = "dst_mac"}, %arg5: !b9i {p4hir.param_name = "port"}) {
          p4hir.call @ipv4_forward (%arg3, %arg4, %arg5) : (!b48i, !b48i, !b9i) -> ()
        }
        p4hir.table_action @drop() {
          p4hir.call @drop () : () -> ()
        }
        p4hir.table_action @NoAction() {
          p4hir.call @NoAction () : () -> ()
        }
      }
      %size = p4hir.table_size #int256_infint
      p4hir.table_default_action {
        p4hir.call @NoAction () : () -> ()
      }
    }
    p4hir.table @int_config_transit {
      p4hir.table_key {
        %val = p4hir.read %arg0 : <!main_headers_t>
        %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
        %srcAddr = p4hir.struct_extract %ipv4["srcAddr"] : !ipv4_t
        p4hir.match_key #exact %srcAddr : !b32i annotations {name = "hdr.ipv4.srcAddr"}
      }
      p4hir.table_actions {
        p4hir.table_action @enable_int_transit(%arg3: !b32i {p4hir.param_name = "switch_id"}) {
          p4hir.call @enable_int_transit (%arg3) : (!b32i) -> ()
        }
        p4hir.table_action @NoAction() {
          p4hir.call @NoAction () : () -> ()
        }
      }
      %size = p4hir.table_size #int64_infint
      p4hir.table_default_action {
        p4hir.call @NoAction () : () -> ()
      }
    }
    p4hir.control_apply {
      %int_node_type_field_ref = p4hir.struct_extract_ref %arg1["int_node_type"] : <!main_metadata_t>
      %c0_b3i = p4hir.const #int0_b3i
      p4hir.assign %c0_b3i, %int_node_type_field_ref : <!b3i>
      %int_enabled_field_ref = p4hir.struct_extract_ref %arg1["int_enabled"] : <!main_metadata_t>
      %c0_b1i = p4hir.const #int0_b1i
      %cast = p4hir.cast(%c0_b1i : !b1i) : !b1i
      p4hir.assign %cast, %int_enabled_field_ref : <!b1i>
      %drop_packet_field_ref = p4hir.struct_extract_ref %arg1["drop_packet"] : <!main_metadata_t>
      %c0_b1i_0 = p4hir.const #int0_b1i
      %cast_1 = p4hir.cast(%c0_b1i_0 : !b1i) : !b1i
      p4hir.assign %cast_1, %drop_packet_field_ref : <!b1i>
      %ipv4_field_ref = p4hir.struct_extract_ref %arg0["ipv4"] : <!main_headers_t>
      %__valid_field_ref = p4hir.struct_extract_ref %ipv4_field_ref["__valid"] : <!ipv4_t>
      %val = p4hir.read %__valid_field_ref : <!validity_bit>
      %valid = p4hir.const #valid
      %eq = p4hir.cmp(eq, %val, %valid) : !validity_bit, !p4hir.bool
      p4hir.if %eq {
        %ipv4_lpm_apply_result = p4hir.table_apply @ipv4_lpm : !ipv4_lpm
        p4hir.call @compute_bloom_hash () : () -> ()
        p4hir.call @read_bloom_counter () : () -> ()
        %val_2 = p4hir.read %arg1 : <!main_metadata_t>
        %bloom_counter = p4hir.struct_extract %val_2["bloom_counter"] : !main_metadata_t
        %c100_b16i = p4hir.const #int100_b16i
        %ge = p4hir.cmp(ge, %bloom_counter, %c100_b16i) : !b16i, !p4hir.bool
        p4hir.if %ge {
          p4hir.call @reset_bloom_counter () : () -> ()
          %int_config_transit_apply_result = p4hir.table_apply @int_config_transit : !int_config_transit
          %val_5 = p4hir.read %arg1 : <!main_metadata_t>
          %int_enabled = p4hir.struct_extract %val_5["int_enabled"] : !main_metadata_t
          %c1 = p4hir.const #int1_infint
          %cast_6 = p4hir.cast(%c1 : !infint) : !b1i
          %eq_7 = p4hir.cmp(eq, %int_enabled, %cast_6) : !b1i, !p4hir.bool
          p4hir.if %eq_7 {
            p4hir.call @add_int_header () : () -> ()
          }
        } else {
          p4hir.call @increment_bloom_counter () : () -> ()
        }
        %ipv4_field_ref_3 = p4hir.struct_extract_ref %arg0["ipv4"] : <!main_headers_t>
        %hdrChecksum_field_ref = p4hir.struct_extract_ref %ipv4_field_ref_3["hdrChecksum"] : <!ipv4_t>
        %c0_b16i = p4hir.const #int0_b16i
        %cast_4 = p4hir.cast(%c0_b16i : !b16i) : !b16i
        p4hir.assign %cast_4, %hdrChecksum_field_ref : <!b16i>
      }
    }
  }
  p4hir.control @MainDeparser(%arg0: !packet_out {p4hir.dir = #undir, p4hir.param_name = "pkt"}, %arg1: !main_headers_t {p4hir.dir = #in, p4hir.param_name = "hdr"}, %arg2: !main_metadata_t {p4hir.dir = #in, p4hir.param_name = "meta"}, %arg3: !nc_standard_metadata_t {p4hir.dir = #in, p4hir.param_name = "standard_meta"})() {
    p4hir.control_apply {
      %ethernet = p4hir.struct_extract %arg1["ethernet"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!ethernet_t]> (%arg0, %ethernet) : !packet_out, (!ethernet_t) -> ()
      %ipv4 = p4hir.struct_extract %arg1["ipv4"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!ipv4_t]> (%arg0, %ipv4) : !packet_out, (!ipv4_t) -> ()
      %tcp = p4hir.struct_extract %arg1["tcp"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!tcp_t]> (%arg0, %tcp) : !packet_out, (!tcp_t) -> ()
      %int_header = p4hir.struct_extract %arg1["int_header"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!int_header_t]> (%arg0, %int_header) : !packet_out, (!int_header_t) -> ()
      %int_switch_id = p4hir.struct_extract %arg1["int_switch_id"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!int_switch_id_t]> (%arg0, %int_switch_id) : !packet_out, (!int_switch_id_t) -> ()
    }
  }
  %MainParser = p4hir.instantiate @MainParser() as "MainParser" : () -> !MainParser
  %MainControl = p4hir.instantiate @MainControl() as "MainControl" : () -> !MainControl
  %MainDeparser = p4hir.instantiate @MainDeparser() as "MainDeparser" : () -> !MainDeparser
  %main = p4hir.instantiate @NC_PIPELINE(%MainParser, %MainControl, %MainDeparser) as "main" : (!MainParser, !MainControl, !MainDeparser) -> !NC_PIPELINE_main_headers_t_main_metadata_t
}
