!b1i = !p4hir.bit<1>
!b32i = !p4hir.bit<32>
!b8i = !p4hir.bit<8>
!b9i = !p4hir.bit<9>
!error = !p4hir.error<NoError, PacketTooShort, NoMatch, StackOutOfBounds, HeaderTooShort, ParserTimeout, ParserInvalidArgument>
!infint = !p4hir.infint
!packet_in = !p4hir.extern<"packet_in">
!packet_out = !p4hir.extern<"packet_out">
!string = !p4hir.string
!type_MH = !p4hir.type_var<"MH">
!type_MM = !p4hir.type_var<"MM">
!type_T = !p4hir.type_var<"T">
!validity_bit = !p4hir.validity.bit
#in = #p4hir<dir in>
#out = #p4hir<dir out>
#undir = #p4hir<dir undir>
!ipv4_t = !p4hir.header<"ipv4_t", protocol: !b8i, src_addr: !b32i, __valid: !validity_bit>
!main_metadata_t = !p4hir.struct<"main_metadata_t", temp: !b32i>
!nc_standard_metadata_t = !p4hir.struct<"nc_standard_metadata_t", ingress_port: !b9i, egress_port: !b9i, packet_length: !b32i, drop: !b1i>
#int0_b32i = #p4hir.int<0> : !b32i
#int255_b32i = #p4hir.int<255> : !b32i
#int4_infint = #p4hir.int<4> : !infint
!MainDeparserT_type_MH_type_MM = !p4hir.control<"MainDeparserT"<!type_MH, !type_MM>, (!packet_out, !type_MH, !type_MM, !nc_standard_metadata_t)>
!MainParserT_type_MH_type_MM = !p4hir.parser<"MainParserT"<!type_MH, !type_MM>, (!packet_in, !p4hir.ref<!type_MH>, !p4hir.ref<!type_MM>, !nc_standard_metadata_t)>
!main_headers_t = !p4hir.struct<"main_headers_t", ipv4: !ipv4_t>
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
  p4hir.package @NC_PIPELINE<[!type_MH, !type_MM]>("main_parser" : !MainParserT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_parser"}, "main_control" : !MainControlT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_control"}, "main_deparser" : !MainDeparserT_type_MH_type_MM {p4hir.dir = #undir, p4hir.param_name = "main_deparser"})
  p4hir.parser @MainParser(%arg0: !packet_in {p4hir.dir = #undir, p4hir.param_name = "pkt"}, %arg1: !p4hir.ref<!main_headers_t> {p4hir.dir = #out, p4hir.param_name = "hdr"}, %arg2: !p4hir.ref<!main_metadata_t> {p4hir.dir = #p4hir<dir inout>, p4hir.param_name = "meta"}, %arg3: !nc_standard_metadata_t {p4hir.dir = #in, p4hir.param_name = "standard_meta"})() {
    p4hir.state @start {
      p4hir.scope {
        %ipv4_field_ref = p4hir.struct_extract_ref %arg1["ipv4"] : <!main_headers_t>
        %hdr_out_arg = p4hir.variable ["hdr_out_arg"] : <!ipv4_t>
        p4hir.call_method @packet_in::@extract<[!ipv4_t]> (%arg0, %hdr_out_arg) : !packet_in, (!p4hir.ref<!ipv4_t>) -> ()
        %val = p4hir.read %hdr_out_arg : <!ipv4_t>
        p4hir.assign %val, %ipv4_field_ref : <!ipv4_t>
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
    p4hir.control_apply {
      %val = p4hir.read %arg0 : <!main_headers_t>
      %ipv4 = p4hir.struct_extract %val["ipv4"] : !main_headers_t
      %protocol = p4hir.struct_extract %ipv4["protocol"] : !ipv4_t
      %c4 = p4hir.const #int4_infint
      %cast = p4hir.cast(%c4 : !infint) : !b8i
      %eq = p4hir.cmp(eq, %protocol, %cast) : !b8i, !p4hir.bool
      p4hir.if %eq {
        %temp_field_ref = p4hir.struct_extract_ref %arg1["temp"] : <!main_metadata_t>
        %val_0 = p4hir.read %arg0 : <!main_headers_t>
        %ipv4_1 = p4hir.struct_extract %val_0["ipv4"] : !main_headers_t
        %src_addr = p4hir.struct_extract %ipv4_1["src_addr"] : !ipv4_t
        %c255_b32i = p4hir.const #int255_b32i
        %and = p4hir.binop(and, %src_addr, %c255_b32i) : !b32i
        p4hir.assign %and, %temp_field_ref : <!b32i>
      } else {
        %temp_field_ref = p4hir.struct_extract_ref %arg1["temp"] : <!main_metadata_t>
        %c0_b32i = p4hir.const #int0_b32i
        %cast_0 = p4hir.cast(%c0_b32i : !b32i) : !b32i
        p4hir.assign %cast_0, %temp_field_ref : <!b32i>
      }
    }
  }
  p4hir.control @MainDeparser(%arg0: !packet_out {p4hir.dir = #undir, p4hir.param_name = "pkt"}, %arg1: !main_headers_t {p4hir.dir = #in, p4hir.param_name = "hdr"}, %arg2: !main_metadata_t {p4hir.dir = #in, p4hir.param_name = "meta"}, %arg3: !nc_standard_metadata_t {p4hir.dir = #in, p4hir.param_name = "standard_meta"})() {
    p4hir.control_apply {
      %ipv4 = p4hir.struct_extract %arg1["ipv4"] : !main_headers_t
      p4hir.call_method @packet_out::@emit<[!ipv4_t]> (%arg0, %ipv4) : !packet_out, (!ipv4_t) -> ()
    }
  }
  %MainParser = p4hir.instantiate @MainParser() as "MainParser" : () -> !MainParser
  %MainControl = p4hir.instantiate @MainControl() as "MainControl" : () -> !MainControl
  %MainDeparser = p4hir.instantiate @MainDeparser() as "MainDeparser" : () -> !MainDeparser
  %main = p4hir.instantiate @NC_PIPELINE(%MainParser, %MainControl, %MainDeparser) as "main" : (!MainParser, !MainControl, !MainDeparser) -> !NC_PIPELINE_main_headers_t_main_metadata_t
}
