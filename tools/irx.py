#!/usr/bin/env python3
"""Disassembler for IOP modules (.irx), with import names.

usage: irx.py <file.irx> imports            list imported functions
       irx.py <file.irx> dis <hexaddr> [n]   disassemble
       irx.py <file.irx> strings            list strings in .rodata/.data
"""
import struct
import sys

import rabbitizer as rz

rz.config.pseudos_enablePseudos = False

# Export tables of the common IOP libraries (index -> name).
LIBS = {
    "sysmem": {4: "AllocSysMemory", 5: "FreeSysMemory", 6: "QueryMemSize", 7: "QueryMaxFreeMemSize",
               8: "QueryTotalFreeMemSize", 9: "QueryBlockTopAddress", 10: "QueryBlockSize", 14: "Kprintf"},
    "loadcore": {5: "FlushDcache", 6: "RegisterLibraryEntries", 7: "ReleaseLibraryEntries",
                 10: "RegisterNonAutoLinkEntries", 12: "QueryLibraryEntryTable", 13: "QueryBootMode"},
    "intrman": {4: "RegisterIntrHandler", 5: "ReleaseIntrHandler", 6: "EnableIntr", 7: "DisableIntr",
                8: "CpuDisableIntr", 9: "CpuEnableIntr", 17: "CpuSuspendIntr", 18: "CpuResumeIntr",
                23: "QueryIntrContext"},
    "thbase": {4: "CreateThread", 5: "DeleteThread", 6: "StartThread", 7: "StartThreadArgs", 8: "ExitThread",
               9: "ExitDeleteThread", 10: "TerminateThread", 11: "iTerminateThread", 12: "DisableDispatchThread",
               13: "EnableDispatchThread", 14: "ChangeThreadPriority", 15: "iChangeThreadPriority",
               16: "RotateThreadReadyQueue", 17: "iRotateThreadReadyQueue", 18: "ReleaseWaitThread",
               19: "iReleaseWaitThread", 20: "GetThreadId", 21: "CheckThreadStack", 22: "ReferThreadStatus",
               23: "iReferThreadStatus", 24: "SleepThread", 25: "WakeupThread", 26: "iWakeupThread",
               27: "CancelWakeupThread", 28: "iCancelWakeupThread", 29: "SuspendThread", 30: "iSuspendThread",
               31: "ResumeThread", 32: "iResumeThread", 33: "DelayThread", 34: "GetSystemTime", 35: "SetAlarm",
               36: "iSetAlarm", 37: "CancelAlarm", 38: "iCancelAlarm", 39: "USec2SysClock", 40: "SysClock2USec",
               41: "GetSystemStatusFlag", 42: "GetThreadCurrentPriority", 43: "GetSystemTimeLow"},
    "thsemap": {4: "CreateSema", 5: "DeleteSema", 6: "SignalSema", 7: "iSignalSema", 8: "WaitSema",
                9: "PollSema", 10: "ReferSemaStatus", 11: "iReferSemaStatus"},
    "thevent": {4: "CreateEventFlag", 5: "DeleteEventFlag", 6: "SetEventFlag", 7: "iSetEventFlag",
                8: "ClearEventFlag", 9: "iClearEventFlag", 10: "WaitEventFlag", 11: "PollEventFlag"},
    "thmsgbx": {4: "CreateMbx", 5: "DeleteMbx", 6: "SendMbx", 7: "iSendMbx", 8: "ReceiveMbx", 9: "PollMbx"},
    "sifman": {5: "sceSifInit", 6: "sceSifSetDChain", 7: "sceSifSetDma", 8: "sceSifDmaStat",
               14: "sceSifSetSIF1DMA? ", 29: "sceSifCheckInit"},
    "sifcmd": {4: "sceSifInitCmd", 5: "sceSifExitCmd", 6: "sceSifGetSreg", 7: "sceSifSetSreg",
               8: "sceSifSetCmdBuffer", 9: "sceSifSetSysCmdBuffer", 10: "sceSifAddCmdHandler",
               11: "sceSifRemoveCmdHandler", 12: "sceSifSendCmd", 13: "isceSifSendCmd", 14: "sceSifInitRpc",
               15: "sceSifBindRpc", 16: "sceSifCallRpc", 17: "sceSifRegisterRpc", 18: "sceSifCheckStatRpc",
               19: "sceSifSetRpcQueue", 20: "sceSifGetNextRequest", 21: "sceSifExecRequest", 22: "sceSifRpcLoop",
               23: "sceSifGetOtherData", 24: "sceSifRemoveRpc", 25: "sceSifRemoveRpcQueue"},
    "stdio": {4: "printf"},
    "sysclib": {4: "setjmp", 5: "longjmp", 6: "toupper", 7: "tolower", 8: "look_ctype_table", 9: "get_ctype_table",
                10: "memchr", 11: "memcmp", 12: "memcpy", 13: "memmove", 14: "memset", 15: "bcmp", 16: "bcopy",
                17: "bzero", 18: "prnt", 19: "sprintf", 20: "strcat", 21: "strchr", 22: "strcmp", 23: "strcpy",
                24: "strcspn", 25: "index", 26: "rindex", 27: "strlen", 28: "strncat", 29: "strncmp",
                30: "strncpy", 31: "strpbrk", 32: "strrchr", 33: "strspn", 34: "strstr", 35: "strtok",
                36: "strtol", 37: "atob", 38: "strtoul", 40: "wmemcopy", 41: "wmemset", 42: "vsprintf"},
    "ioman": {4: "open", 5: "close", 6: "read", 7: "write", 8: "lseek", 9: "ioctl", 10: "remove", 11: "mkdir",
              12: "rmdir", 13: "dopen", 14: "dclose", 15: "dread", 16: "getstat", 17: "chstat", 18: "format",
              20: "AddDrv", 21: "DelDrv"},
    "cdvdman": {4: "sceCdInit", 5: "sceCdStandby", 6: "sceCdRead", 7: "sceCdSeek", 8: "sceCdGetError",
                9: "sceCdGetToc", 10: "sceCdSearchFile", 11: "sceCdSync", 12: "sceCdGetDiskType",
                13: "sceCdDiskReady", 14: "sceCdTrayReq", 15: "sceCdStop", 16: "sceCdPosToInt",
                17: "sceCdIntToPos", 21: "sceCdCheckCmd", 22: "_sceCdRI", 24: "sceCdReadClock",
                28: "sceCdStatus", 29: "sceCdApplySCmd", 37: "sceCdCallback", 38: "sceCdPause",
                39: "sceCdBreak", 40: "sceCdReadCdda", 44: "sceCdGetReadPos", 45: "sceCdCtrlADout",
                46: "sceCdNop", 47: "_sceGetFsvRbuf", 48: "_sceCdstm0Cb", 49: "_sceCdstm1Cb", 50: "_sceCdSC",
                51: "_sceCdRC", 52: "sceCdForbidDVDP", 53: "sceCdReadSubQ", 54: "sceCdApplyNCmd",
                55: "AutoAdjustCtrl", 56: "sceCdStInit", 57: "sceCdStRead", 58: "sceCdStSeek",
                59: "sceCdStStart", 60: "sceCdStStat", 61: "sceCdStStop", 62: "sceCdRead0",
                63: "_sceCdRV", 64: "_sceCdRM", 66: "sceCdReadChain", 67: "sceCdStPause",
                68: "sceCdStResume", 74: "sceCdPowerOff", 75: "sceCdMmode", 77: "sceCdStSeekF",
                78: "sceCdPOffCallback", 83: "sceCdReadDvdDualInfo", 84: "sceCdLayerSearchFile",
                112: "sceCdStatus2"},
    "libsd": {2: "sceSdQuit", 4: "sceSdInit", 5: "sceSdSetParam", 6: "sceSdGetParam", 7: "sceSdSetSwitch",
              8: "sceSdGetSwitch", 9: "sceSdSetAddr", 10: "sceSdGetAddr", 11: "sceSdSetCoreAttr",
              12: "sceSdGetCoreAttr", 13: "sceSdNote2Pitch", 14: "sceSdPitch2Note", 15: "sceSdProcBatch",
              16: "sceSdProcBatchEx", 17: "sceSdVoiceTrans", 18: "sceSdBlockTrans", 19: "sceSdVoiceTransStatus",
              20: "sceSdBlockTransStatus", 21: "sceSdSetTransCallback", 22: "sceSdSetIRQCallback",
              23: "sceSdSetEffectAttr", 24: "sceSdGetEffectAttr", 25: "sceSdClearEffectWorkArea",
              26: "sceSdSetTransIntrHandler", 27: "sceSdSetSpu2IntrHandler", 28: "sceSdGetTransIntrHandlerArgument",
              29: "sceSdGetSpu2IntrHandlerArgument", 30: "sceSdStopTrans", 31: "sceSdCleanEffectWorkArea",
              32: "sceSdSetEffectMode", 33: "sceSdSetEffectModeParams"},
    "timrman": {4: "AllocHardTimer", 5: "ReferHardTimer", 6: "FreeHardTimer", 7: "SetTimerMode",
                8: "GetTimerStatus", 9: "SetTimerCounter", 10: "GetTimerCounter", 11: "SetTimerCompare",
                12: "GetTimerCompare", 13: "SetHoldMode", 14: "GetHoldMode", 15: "GetHoldReg",
                16: "GetHardTimerIntrCode", 20: "SetTimerHandler", 21: "SetOverflowHandler"},
    "vblank": {4: "WaitVblankStart", 5: "WaitVblankEnd", 6: "WaitVblank", 7: "WaitNonVblank",
               8: "RegisterVblankHandler", 9: "ReleaseVblankHandler"},
    "modload": {4: "ReBootStart", 5: "LoadModuleAddress", 6: "LoadModule", 7: "LoadStartModule",
                8: "StartModule", 9: "LoadModuleBufferAddress", 10: "LoadModuleBuffer"},
}


class Irx:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
        stro = secs[shstrndx][4]
        self.mem = bytearray(0x40000)
        self.text_size = 0
        for (nm, typ, flg, addr, off, size, *_r) in secs:
            name = d[stro + nm:d.index(b"\0", stro + nm)].decode()
            if typ == 1 and flg & 2:  # PROGBITS alloc
                self.mem[addr:addr + size] = d[off:off + size]
                if name == ".text":
                    self.text_size = size
            if name in (".rodata", ".data"):
                setattr(self, name[1:], (addr, size))
        self.imports = {}  # stub address -> name
        self.find_imports()

    def w(self, a):
        return struct.unpack_from("<I", self.mem, a)[0]

    def find_imports(self):
        a = 0
        while a < self.text_size:
            if self.w(a) == 0x41E00000 and self.w(a + 4) == 0:
                name = bytes(self.mem[a + 12:a + 20]).split(b"\0")[0].decode(errors="replace")
                p = a + 20
                while self.w(p) == 0x03E00008:
                    n = self.w(p + 4) & 0xFFFF
                    fn = LIBS.get(name, {}).get(n, "%s_%d" % (name, n))
                    self.imports[p] = fn
                    p += 8
                a = p
            else:
                a += 4

    def string(self, a):
        if a >= len(self.mem):
            return None
        e = self.mem.find(b"\0", a, a + 120)
        s = bytes(self.mem[a:e]) if e > a else b""
        if len(s) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in s):
            return s.decode().replace("\n", "\\n")
        return None

    def dis(self, a, n):
        hi = {}
        for k in range(n):
            va = a + 4 * k
            i = rz.Instruction(self.w(va), va, rz.InstrCategory.CPU)
            op = i.getOpcodeName()
            note = ""
            if op == "jal":
                t = i.getInstrIndexAsVram()
                note = self.imports.get(t, "sub_%05x" % t)
            elif op == "lui":
                hi[i.rt.value] = (i.getImmediate() << 16) & 0xFFFFFFFF
            elif op in ("addiu", "ori") and i.rs.value in hi:
                v = (hi[i.rs.value] + (i.getProcessedImmediate() if op == "addiu" else i.getImmediate())) & 0xFFFFFFFF
                s = self.string(v) if v < 0x40000 else None
                note = ('"%s"' % s) if s else "&%05x" % v
            lab = ("%s:\n" % self.imports[va]) if va in self.imports else ""
            print("%s%05x %08x %-40s %s" % (lab, va, self.w(va), i.disassemble(), ("; " + note) if note else ""))


if __name__ == "__main__":
    x = Irx(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "imports":
        for a, n in sorted(x.imports.items()):
            print("%05x %s" % (a, n))
    elif cmd == "dis":
        x.dis(int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 60)
    elif cmd == "strings":
        for sec in ("rodata", "data"):
            a0, sz = getattr(x, sec)
            a = a0
            while a < a0 + sz:
                s = x.string(a)
                if s:
                    print("%05x %s" % (a, s))
                    a += len(s) + 1
                else:
                    a += 1
