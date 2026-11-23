#!/usr/bin/env python3
"""Execute the production capped-log append/rotation and orphan recovery.

Real Arduino String and Print exercise CRLF and write-count behavior. The file,
VFS, allocation and lock boundaries are controlled; this does not qualify media
or power-loss durability. Optional saved sources establish unchanged output and
characterize the old false-success/data-loss faults separately.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CORE = COMPONENT.parent / "arduino/cores/esp32"


def generate(overlay: Path | None, baseline: bool | None = None) -> str:
    """The harness on `overlay`'s sources (default: this tree). `baseline`
    (default: an overlay is given) selects the saved pre-G-39 expectations."""
    if baseline is None:
        baseline = overlay is not None
    source = (overlay or COMPONENT) / "System_Filesystem.cpp"
    production = source.read_text()
    header = ((overlay or COMPONENT) / "System_Filesystem.h").read_text()
    wrapper = "bool appendLineWithCap(const char* path, const String& line, size_t capBytes) {"
    string_form = ""
    begin, end = "// --- capped-log writer: begin", "// --- capped-log writer: end ---"
    batching = begin in production
    if batching:
        # S-8: the writer is one region (batch state, append, both rotations,
        # the batch buffers and their flushes); the orphan recovery follows it.
        require(production.count(begin) == 1 and production.count(end) == 1, "one capped-log writer region")
        functions = production[production.index(begin):production.index(end)]
        functions += "\n\n" + extract_block(production, "static void recoverLogOrphanAt(")
        functions += "\n\n" + extract_block(production, "void cleanupLogOrphan(")
        string_form = extract_block(header, "inline " + wrapper[:-2])
    else:
        markers = [marker for marker in ("static bool appendCappedLineOnce(", "static bool restartCappedLog(")
                   if marker in production]
        markers.append("bool appendLineWithCap(")
        # The String form, a wrapper around the (text, length) form since C1: out of
        # line in System_Filesystem.cpp at first, inline in System_Filesystem.h since.
        if "bool appendLineWithCap(const char* path, const char* line, size_t lineLength" in production:
            if wrapper in production:
                markers.append(wrapper[:-2])
            elif "inline " + wrapper in header:
                string_form = extract_block(header, "inline " + wrapper[:-2])
        if "static void recoverLogOrphanAt(" in production:
            markers.append("static void recoverLogOrphanAt(")
        markers.append("void cleanupLogOrphan(")
        functions = "\n\n".join(extract_block(production, marker) for marker in markers)
    if string_form:
        functions += "\n\n" + string_form
    # log_out's capped-log line (System_Debug.cpp): the timestamp prefix and,
    # since C1, the line formatted into its kept PSRAM block.
    debug = ((overlay or COMPONENT) / "System_Debug.cpp").read_text()
    timestamped = "static void appendTimestampedLine(const char* path, const char* text, size_t capBytes) {"
    if timestamped in debug:
        start = debug.index("static constexpr size_t kTimestampPrefixSize")
        definition = debug.index(timestamped)  # not the forward declaration
        end = definition + len(extract_block(debug[definition:], "static void appendTimestampedLine("))
        line_code = "#define HAVE_TIMESTAMPED_LINE 1\n" + debug[start:end]
    else:
        line_code = "#define HAVE_TIMESTAMPED_LINE 0\n" + extract_block(debug, "static String buildTimestampPrefix(")
    harness = (HERE / "capped_log_writes_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one production insertion marker")
    require(harness.count("// INSERT_LOG_LINE") == 1, "one log line insertion marker")
    require(harness.count("// INSERT_SEALED_LOG") == 1, "one sealed log insertion marker")
    require(harness.count("// INSERT_CAPCRYPT_MACROS") == 1, "one capture crypto macro insertion marker")
    notes = int("catchupNoteTrim(" in production)
    # B11: a writer that seals on the card is compiled with the production
    # System_SealedLog (header and source, their includes dropped). Its one
    # line changed here is appliesTo, which also asks the harness's switch, so
    # the cases from before B11 keep their plain bytes on either tier.
    sealed, macros = "", ""
    sealing = "SealedLog::" in functions
    if sealing:
        strip = lambda text: re.sub(r"(?m)^#(include|ifndef SYSTEM_SEALED_LOG_H|define SYSTEM_SEALED_LOG_H)[^\n]*\n", "", text)
        header = strip(((overlay or COMPONENT) / "System_SealedLog.h").read_text())
        header = header[:header.rindex("#endif")]
        source_text = strip(((overlay or COMPONENT) / "System_SealedLog.cpp").read_text())
        applies = "bool appliesTo(const String& path) { return VFS::getStorageType(path) == VFS::SDCARD; }"
        require(source_text.count(applies) == 1, "SealedLog::appliesTo is the card test")
        source_text = source_text.replace(
            applies, "bool appliesTo(const String& path) { return cardSealing && VFS::getStorageType(path) == VFS::SDCARD; }")
        sealed = header + "\n" + source_text
        crypto_header = ((overlay or COMPONENT) / "System_CaptureCrypto.h").read_text()
        macros = "\n".join(re.findall(r"^#define CAPCRYPT_.*$", crypto_header, re.MULTILINE))
    return (f"#define BASELINE {int(baseline)}\n#define HAVE_CATCHUP_NOTES {notes}\n"
            f"#define HAVE_BATCHING {int(batching)}\n#define HAVE_SEALED_LOG {int(sealing)}\n" +
            harness.replace("// INSERT_PRODUCTION", functions).replace("// INSERT_LOG_LINE", line_code)
            .replace("// INSERT_SEALED_LOG", sealed).replace("// INSERT_CAPCRYPT_MACROS", macros))


def source_contract() -> None:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    task = extract_block(debug, "void debugOutputTask(")
    # The [ERROR], [EVENT] and [EVLOG] lines go through the kept line block.
    for log in ("LOG_ERROR_FILE", "LOG_EVENTS_FILE", "LOG_EVENT_STREAM_FILE"):
        require(f"appendTimestampedLine({log}, msg->text," in task, f"log_out appends {log} through appendTimestampedLine")
    require("buildTimestampPrefix()" not in task and "appendLineWithCap(" not in task,
            "log_out builds no String line of its own")
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    require('appendLineWithCap("/system/sys_logs/command-audit.log", entry, strlen(entry), 500 * 1024);' in utils,
            "the command audit hands its char buffer on without a String")
    fs = (COMPONENT / "System_Filesystem.cpp").read_text()
    body = extract_block(fs, "static bool cappedLogWrite(")
    require("String(dest)" not in body and "String(tmpPath)" not in body,
            "the capped-log write makes the resolved path's String once per tier")
    # File catch-up (System_CatchupLedger.h): one trim note after the promoted
    # rename, one restart note after the restarted log opened; the hooks do no
    # I/O, logging or allocation (they run inside this writer).
    require(body.count("catchupNoteTrim(") == 1 and
            body.count("catchupNoteTrim(dest, tailStart > markBytes ? tailStart - markBytes : 0);") == 1 and
            body.index("catchupNoteTrim(") > body.index("renameGuarded(tmpName, destPath, ctx)") and
            body.index("catchupNoteTrim(") > body.index('STACK_TRACEF("appendLineWithCap.rename_failed")'),
            "the copy rotation notes the trim once, after the successful rename (less the mark line of a card copy)")
    # S-8: the two-generation rotation leaves no note (the logs the ledger
    # follows never take it in a build that serves catch-up) and copies nothing
    # of its own: one rename, with the staged copy only for an oversized file.
    generation = extract_block(fs, "static bool rotateCappedLogByRename(")
    require("catchupNote" not in generation and "readBytes" not in generation and ".write(" not in generation,
            "the generation rotation writes no data and leaves no catch-up note")
    require("if (!copyRotation) return rotateCappedLogByRename(destPath, sz, limit, ctx, copyAllowed);" in body and
            "const size_t limit = copyRotation ? capBytes : logcaps::generationBytes(capBytes);" in body,
            "a two-generation log rotates at half its cap, by rename")
    # Boot does not wait for a copy this firmware brings: until the main loop
    # runs, a file is copied only above the cap its caller names (where older
    # firmware rotated too). The loop's one tick line marks the end of boot.
    require("const bool copyAllowed = logcaps::copyAllowedNow(sCappedLogs.loopRunning, sz, nominalCap);" in body and
            "if (!copyAllowed) {" in body and
            body.index("if (!copyAllowed) {") < body.index("stageCappedLogTail(destPath, sz, (capBytes * 85) / 100"),
            "the copy rotation is put off until boot is over")
    require("? logcaps::cardCap(nominalCap)" in body and
            ": logcaps::scaledCap(nominalCap, sCappedLogs.partitionBytes);" in body and
            body.index("const size_t capBytes = SealedLog::appliesTo(destPath)") >
            body.index('STACK_TRACEF("appendLineWithCap.short_append")'),
            "the writer scales the caller's cap itself, by the tier the line landed on: the flash share, "
            "or twice the cap in stored bytes on the card (B11)")
    # B11: nothing plain reaches the card. The append, the restart and the
    # copy rotation each go through SealedLog for a card path.
    once = extract_block(fs, "static bool appendCappedLineOnce(")
    require(once.index("if (SealedLog::appliesTo(dest)) return appendCappedSealedOnce(") <
            once.index('VFS::openGuarded(dest, "a", ctx, true)'),
            "a card path is appended sealed before any plain open")
    sealed_once = extract_block(fs, "static bool appendCappedSealedOnce(")
    require("SealedLog::openAppend(dest, ctx, stored)" in sealed_once and
            "SealedLog::writeLines(a, data, length, stored)" in sealed_once and
            'SealedLog::writeLines(a, "\\n", 1, stored)' in sealed_once and
            "VFS::openGuarded" not in sealed_once and ".println(" not in sealed_once,
            "the sealed append opens and writes through SealedLog only")
    stage = extract_block(fs, "static LogTailStage stageCappedLogTail(")
    require(stage.index("SealedLog::writeMark(w, markBytes)") < stage.index("while (copied < tailBytes)") and
            "w.size() == markBytes + tailBytes" in stage,
            "a rotation's copy on the card starts with the mark, and its size check counts it")
    require("if (oversized && !copyAllowed) {" in generation and
            generation.index("if (oversized && !copyAllowed) {") < generation.index("stageCappedLogTail("),
            "the one-time cut of an oversized file is put off until boot is over")
    tick = extract_block((COMPONENT / "System_VFS.cpp").read_text(), "void sdWatchTick()")
    require(tick.index("cappedLogsNoteLoopRunning();") < tick.index("#if HW1_SD_CARD_WATCH"),
            "the main loop's tick marks the end of boot on every board")
    require("VFS::sdWatchTick();" in extract_block((COMPONENT / "HardwareOne.cpp").read_text(), "void hardwareone_loop()"),
            "and that tick is in the main loop, not in setup")
    require("return kCatchupServesLogs && logcaps::followedByCatchup(path);" in
            extract_block(fs, "static bool cappedLogKeepsCopyRotation("),
            "only the followed logs of a serving build keep the copy rotation")
    require(re.search(r"#if ENABLE_ESPNOW_SEND\nstatic const bool kCatchupServesLogs = true;\n#else\n"
                      r"static const bool kCatchupServesLogs = false;\n#endif", fs) is not None,
            "serving catch-up is the ENABLE_ESPNOW_SEND build")
    # S-8 batching: the entry point batches or writes through; the cap is
    # scaled to the partition (UP-03) in both paths.
    entry = extract_block(fs, "bool appendLineWithCap(const char* path, const char* line, size_t lineLength,")
    require(entry.index("if (cappedLogBatchLine(path, line, lineLength, capBytes)) return true;") <
            entry.index('FsLockGuard guard("appendLineWithCap");'),
            "a batched line returns before the filesystem lock is taken")
    require("return cappedLogWrite(path, line, lineLength, true, 1, capBytes);" in entry and
            "batch, length, false, lines,\n                                 capBytes);" in
            extract_block(fs, "static bool cappedLogFlushSlotLocked(") and
            fs.count("logcaps::scaledCap(") == 2,
            "both write paths hand the caller's cap to the one place that scales it")
    init = re.sub(r"//[^\n]*", "", extract_block(fs, "void cappedLogsInit("))
    require("esp_partition_find_first(" in init and "esp_littlefs_info" not in init and "totalBytes" not in init,
            "the cap total is the partition size, never a volume walk")
    require("AllocPref::RequirePsram" in init and "PreferPsram" not in init and
            "xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)" in init,
            "batch memory and its mutex are PSRAM only")
    boot = extract_block(fs, "bool initFilesystem(")
    require(boot.index("VFS::init();") < boot.index("cappedLogsInit();") < boot.index("cleanupLogOrphan(LOG_OK_FILE);"),
            "the writer is set up at mount, before the first capped-log call")
    require(boot.count("cleanupLogOrphan(") == 6, "boot recovery checks the same six logs as before (boot does not grow)")
    # The log task: a timed wait, the due flush on every pass, and the flush
    # that latches write-through when the restart line is written.
    require("xQueueReceive(gDebugOutputQueue, &msg, pdMS_TO_TICKS(logcaps::kBatchTickMs))" in task and
            "portMAX_DELAY" not in task, "log_out wakes every 500 ms while idle")
    require("if (filesystemReady) cappedLogsFlushDue();" in task, "log_out writes the due batches on every pass")
    events_append = task.index("appendTimestampedLine(LOG_EVENTS_FILE, msg->text, LOG_EVENTS_CAP);")
    reboot = 'if (strncmp(msg->text, "[EVENT][REBOOT]", 15) == 0) cappedLogsFlush(true);'
    require(reboot in task and task.index(reboot) > events_append,
            "the restart line is appended, then everything is flushed and written through")
    require('logSystemEvent("REBOOT",' in extract_block(utils, "void recordRebootIntent("),
            "every planned restart writes the [EVENT][REBOOT] line the log task acts on")
    vfs = (COMPONENT / "System_VFS.cpp").read_text()
    opened = extract_block(vfs, "File openGuarded(")
    require(opened.index("if (needsRead) flushCappedLogsForReader(norm, ctx);") < opened.index("return open(norm, mode, create);"),
            "a read open flushes the capped logs first")
    hook = extract_block(vfs, "static void flushCappedLogsForReader(")
    require("isSystemIdentity(ctx)" in hook and "cappedLogsFlush(false);" in hook and "sys_logs" in hook,
            "only a user's read under the system log folders flushes; system identities never do")
    restart = extract_block(fs, "static bool restartCappedLog(")
    require(restart.count("catchupNoteRestart(dest.c_str());") == 2 and
            restart.index("catchupNoteRestart(") > restart.index("restart_open_failed") and
            restart.rindex("catchupNoteRestart(") > restart.rindex("restart_open_failed"),
            "restartCappedLog notes the restart once per tier's branch, after the new log opened")
    require(restart.index("if (SealedLog::appliesTo(dest)) {") < restart.index("} else {") <
            restart.index('VFS::openGuarded(dest, "w", ctx, true)'),
            "a restarted log on the card is a sealed file; the plain restart is for flash only")
    ledger_path = COMPONENT / "System_CatchupLedger.cpp"
    if ledger_path.exists():
        ledger = ledger_path.read_text()
        for hook in ("void catchupNoteTrim(", "void catchupNoteRestart("):
            hook_body = re.sub(r"//[^\n]*", "", extract_block(ledger, hook))
            for banned in ("VFS::", "File ", "WARN_", "DEBUG_", "CL_WARN", "CL_DEBUG", "alloc", "Serial", "printf"):
                require(banned not in hook_body, f"{hook.strip('(')} must not use {banned}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component sources; also runs current")
    parser.add_argument("--evidence", type=Path, help="retain generated source and output")
    parser.add_argument("--previous", type=Path,
                        help="a component tree of an earlier commit (same expectations); its transcript must equal this one's")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-capped-logs-") as temp:
        work = args.evidence.resolve() if args.evidence else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        # The core String with its heap calls counted (arduino_string_counted):
        # the log-line cases count the blocks a line costs.
        string = (CORE / "WString.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdio>')
        string = string.replace('#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
        string = re.sub(r'\b(malloc|realloc|free)\(', lambda m: 'hostString' + m[1].capitalize() + '(', string)
        (work / "WString.cpp").write_text('#include "string_heap_count.h"\n' + string)
        (work / "string_heap_count.h").write_text(
            '#pragma once\n#include <cstddef>\n'
            'struct StringHeapCount { unsigned long made = 0, regrown = 0, freed = 0; };\n'
            'extern StringHeapCount gStringHeap;\n'
            'void* hostStringMalloc(size_t);\nvoid* hostStringRealloc(void*, size_t);\nvoid hostStringFree(void*);\n')
        (work / "string_heap_count.cpp").write_text(
            '#include "string_heap_count.h"\n#include <cstdlib>\nStringHeapCount gStringHeap;\n'
            'void* hostStringMalloc(size_t n) { ++gStringHeap.made; return std::malloc(n); }\n'
            'void* hostStringRealloc(void* p, size_t n) { if (p) ++gStringHeap.regrown; else ++gStringHeap.made; '
            'return std::realloc(p, n); }\n'
            'void hostStringFree(void* p) { if (p) ++gStringHeap.freed; std::free(p); }\n')
        # The pure policy header the writer region and the harness both use.
        shutil.copyfile(COMPONENT / "System_LogCapsCore.h", work / "System_LogCapsCore.h")
        shutil.copyfile(COMPONENT / "System_SealedLogCore.h", work / "System_SealedLogCore.h")
        printing = (CORE / "Print.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdarg>')
        (work / "Print.cpp").write_text(printing)
        conversions = (CORE / "stdlib_noniso.c").read_text().replace('#include "esp_system.h"', '')
        conversions += "\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\nchar *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n"
        (work / "stdlib_noniso.c").write_text(conversions)
        sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-I", str(work), "-I", str(CORE), *sanitize]
        subprocess.run([shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CORE),
                        "-c", str(work / "stdlib_noniso.c"), "-o", str(work / "conversions.o")], check=True)
        for source in ("WString", "Print", "string_heap_count"):
            subprocess.run([args.cxx, *flags, "-Wno-sign-compare", "-c", str(work / f"{source}.cpp"),
                            "-o", str(work / f"{source}.o")], check=True)
        transcripts = {}
        versions = [("baseline", args.baseline, True)] if args.baseline else []
        versions += [("previous", args.previous, False)] if args.previous else []
        for label, source, saved in [*versions, ("current", None, False)]:
            unit, binary = work / f"{label}.cpp", work / label
            unit.write_text(generate(source, saved))
            subprocess.run([args.cxx, *flags, str(unit), str(work / "WString.o"), str(work / "Print.o"),
                            str(work / "string_heap_count.o"), str(work / "conversions.o"), "-o", str(binary)],
                           check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            (work / f"{label}-transcript.txt").write_text(result.stdout)
            (work / f"{label}-assertions.log").write_text(result.stderr)
            require(result.returncode == 0, f"{label}: {result.stderr}")
            transcripts[label] = result.stdout
            print(f"{label}: {result.stderr.strip()}")
        if args.previous:
            require(transcripts["previous"] == transcripts["current"], "previous/current transcripts differ")
            print(f"previous tree: {len(transcripts['current'].splitlines())} transcript lines equal")
        if args.baseline:
            require(transcripts["baseline"] == transcripts["current"], "unchanged baseline/current outputs differ")
            print("Normal output is unchanged; saved baseline reproduces old append, copy and promotion faults.")
    source_contract()


if __name__ == "__main__":
    main()
