; ModuleID = '/local/yihan/nutcracker/apps/ingressMazuNAT/ingressMazuNAT.c'
source_filename = "/local/yihan/nutcracker/apps/ingressMazuNAT/ingressMazuNAT.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.packet = type { %struct.ethernet, %struct.ipv4, %struct.tcp, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }
%struct.ethernet = type { i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i16 }
%struct.ipv4 = type { i8, i8, i8, i8, i8, i8, i8, i8, i16, i32, i32, i32, i32 }
%struct.tcp = type { i16, i16, i32, i32, i8, i8, i16, i16, i16, i32, i32, i32, i32 }

; Function Attrs: noinline nounwind optnone uwtable
define dso_local i32 @process_inbound(ptr noundef %pkt) #0 section ".entry" {
entry:
  %pkt.addr = alloca ptr, align 8
  %ipv4 = alloca ptr, align 8
  %tcp = alloca ptr, align 8
  %dst_addr = alloca i32, align 4
  %dst_port = alloca i16, align 2
  %protocol = alloca i8, align 1
  store ptr %pkt, ptr %pkt.addr, align 8
  %0 = load ptr, ptr %pkt.addr, align 8
  %ipv41 = getelementptr inbounds nuw %struct.packet, ptr %0, i32 0, i32 1
  store ptr %ipv41, ptr %ipv4, align 8
  %1 = load ptr, ptr %pkt.addr, align 8
  %tcp2 = getelementptr inbounds nuw %struct.packet, ptr %1, i32 0, i32 2
  store ptr %tcp2, ptr %tcp, align 8
  %2 = load ptr, ptr %ipv4, align 8
  %dstAddr = getelementptr inbounds nuw %struct.ipv4, ptr %2, i32 0, i32 10
  %3 = load i32, ptr %dstAddr, align 4
  store i32 %3, ptr %dst_addr, align 4
  %4 = load ptr, ptr %tcp, align 8
  %dstPort = getelementptr inbounds nuw %struct.tcp, ptr %4, i32 0, i32 1
  %5 = load i16, ptr %dstPort, align 2
  store i16 %5, ptr %dst_port, align 2
  %6 = load ptr, ptr %ipv4, align 8
  %protocol3 = getelementptr inbounds nuw %struct.ipv4, ptr %6, i32 0, i32 7
  %7 = load i8, ptr %protocol3, align 1
  store i8 %7, ptr %protocol, align 1
  %8 = load i32, ptr %dst_addr, align 4
  %call = call i32 @htonl(i32 noundef -1062731519) #2
  %cmp = icmp eq i32 %8, %call
  br i1 %cmp, label %if.then, label %if.end19

if.then:                                          ; preds = %entry
  %9 = load i8, ptr %protocol, align 1
  %conv = zext i8 %9 to i32
  %cmp4 = icmp eq i32 %conv, 6
  br i1 %cmp4, label %land.lhs.true, label %if.end

land.lhs.true:                                    ; preds = %if.then
  %10 = load i16, ptr %dst_port, align 2
  %conv6 = zext i16 %10 to i32
  %call7 = call zeroext i16 @htons(i16 noundef zeroext 443) #2
  %conv8 = zext i16 %call7 to i32
  %cmp9 = icmp eq i32 %conv6, %conv8
  br i1 %cmp9, label %if.then16, label %lor.lhs.false

lor.lhs.false:                                    ; preds = %land.lhs.true
  %11 = load i16, ptr %dst_port, align 2
  %conv11 = zext i16 %11 to i32
  %call12 = call zeroext i16 @htons(i16 noundef zeroext 80) #2
  %conv13 = zext i16 %call12 to i32
  %cmp14 = icmp eq i32 %conv11, %conv13
  br i1 %cmp14, label %if.then16, label %if.end

if.then16:                                        ; preds = %lor.lhs.false, %land.lhs.true
  %call17 = call i32 @htonl(i32 noundef 167772170) #2
  %12 = load ptr, ptr %ipv4, align 8
  %dstAddr18 = getelementptr inbounds nuw %struct.ipv4, ptr %12, i32 0, i32 10
  store i32 %call17, ptr %dstAddr18, align 4
  br label %if.end

if.end:                                           ; preds = %if.then16, %lor.lhs.false, %if.then
  br label %if.end19

if.end19:                                         ; preds = %if.end, %entry
  %13 = load ptr, ptr %pkt.addr, align 8
  %ethernet = getelementptr inbounds nuw %struct.packet, ptr %13, i32 0, i32 0
  %srcAddr_0 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet, i32 0, i32 6
  store i8 60, ptr %srcAddr_0, align 2
  %14 = load ptr, ptr %pkt.addr, align 8
  %ethernet20 = getelementptr inbounds nuw %struct.packet, ptr %14, i32 0, i32 0
  %srcAddr_1 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet20, i32 0, i32 7
  store i8 -3, ptr %srcAddr_1, align 1
  %15 = load ptr, ptr %pkt.addr, align 8
  %ethernet21 = getelementptr inbounds nuw %struct.packet, ptr %15, i32 0, i32 0
  %srcAddr_2 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet21, i32 0, i32 8
  store i8 -2, ptr %srcAddr_2, align 4
  %16 = load ptr, ptr %pkt.addr, align 8
  %ethernet22 = getelementptr inbounds nuw %struct.packet, ptr %16, i32 0, i32 0
  %srcAddr_3 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet22, i32 0, i32 9
  store i8 -98, ptr %srcAddr_3, align 1
  %17 = load ptr, ptr %pkt.addr, align 8
  %ethernet23 = getelementptr inbounds nuw %struct.packet, ptr %17, i32 0, i32 0
  %srcAddr_4 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet23, i32 0, i32 10
  store i8 125, ptr %srcAddr_4, align 2
  %18 = load ptr, ptr %pkt.addr, align 8
  %ethernet24 = getelementptr inbounds nuw %struct.packet, ptr %18, i32 0, i32 0
  %srcAddr_5 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet24, i32 0, i32 11
  store i8 33, ptr %srcAddr_5, align 1
  %19 = load ptr, ptr %pkt.addr, align 8
  %ethernet25 = getelementptr inbounds nuw %struct.packet, ptr %19, i32 0, i32 0
  %dstAddr_0 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet25, i32 0, i32 0
  store i8 60, ptr %dstAddr_0, align 4
  %20 = load ptr, ptr %pkt.addr, align 8
  %ethernet26 = getelementptr inbounds nuw %struct.packet, ptr %20, i32 0, i32 0
  %dstAddr_1 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet26, i32 0, i32 1
  store i8 -3, ptr %dstAddr_1, align 1
  %21 = load ptr, ptr %pkt.addr, align 8
  %ethernet27 = getelementptr inbounds nuw %struct.packet, ptr %21, i32 0, i32 0
  %dstAddr_2 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet27, i32 0, i32 2
  store i8 -2, ptr %dstAddr_2, align 2
  %22 = load ptr, ptr %pkt.addr, align 8
  %ethernet28 = getelementptr inbounds nuw %struct.packet, ptr %22, i32 0, i32 0
  %dstAddr_3 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet28, i32 0, i32 3
  store i8 -98, ptr %dstAddr_3, align 1
  %23 = load ptr, ptr %pkt.addr, align 8
  %ethernet29 = getelementptr inbounds nuw %struct.packet, ptr %23, i32 0, i32 0
  %dstAddr_4 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet29, i32 0, i32 4
  store i8 93, ptr %dstAddr_4, align 4
  %24 = load ptr, ptr %pkt.addr, align 8
  %ethernet30 = getelementptr inbounds nuw %struct.packet, ptr %24, i32 0, i32 0
  %dstAddr_5 = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet30, i32 0, i32 5
  store i8 1, ptr %dstAddr_5, align 1
  %call31 = call zeroext i16 @htons(i16 noundef zeroext 2048) #2
  %25 = load ptr, ptr %pkt.addr, align 8
  %ethernet32 = getelementptr inbounds nuw %struct.packet, ptr %25, i32 0, i32 0
  %etherType = getelementptr inbounds nuw %struct.ethernet, ptr %ethernet32, i32 0, i32 12
  store i16 %call31, ptr %etherType, align 4
  ret i32 0
}

; Function Attrs: nounwind willreturn memory(none)
declare i32 @htonl(i32 noundef) #1

; Function Attrs: nounwind willreturn memory(none)
declare zeroext i16 @htons(i16 noundef zeroext) #1

; Function Attrs: noinline nounwind optnone uwtable
define dso_local i32 @main() #0 {
entry:
  %retval = alloca i32, align 4
  store i32 0, ptr %retval, align 4
  %call = call i32 @process_inbound(ptr noundef null)
  ret i32 0
}

attributes #0 = { noinline nounwind optnone uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nounwind willreturn memory(none) "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { nounwind willreturn memory(none) }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{i32 7, !"frame-pointer", i32 2}
!5 = !{!"clang version 20.1.1 (https://github.com/llvm/llvm-project.git 424c2d9b7e4de40d0804dd374721e6411c27d1d1)"}
