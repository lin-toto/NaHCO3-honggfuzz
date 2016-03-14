/*
 *
 * honggfuzz - architecture dependent code (POSIX / SIGNAL)
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "common.h"
#include "arch.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "files.h"
#include "util.h"
#include "sancov.h"

#ifdef __ANDROID__
#ifndef WIFCONTINUED
#define WIFCONTINUED(x) WEXITSTATUS(0)
#endif
#endif

/*  *INDENT-OFF* */
struct {
    bool important;
    const char *descr;
} arch_sigs[NSIG] = {
    [0 ... (NSIG - 1)].important = false,
    [0 ... (NSIG - 1)].descr = "UNKNOWN",

    [SIGILL].important = true,
    [SIGILL].descr = "SIGILL",
    [SIGFPE].important = true,
    [SIGFPE].descr = "SIGFPE",
    [SIGSEGV].important = true,
    [SIGSEGV].descr = "SIGSEGV",
    [SIGBUS].important = true,
    [SIGBUS].descr = "SIGBUS",
    [SIGABRT].important = true,
    [SIGABRT].descr = "SIGABRT"
};
/*  *INDENT-ON* */

/*
 * Returns true if a process exited (so, presumably, we can delete an input
 * file)
 */
static bool arch_analyzeSignal(honggfuzz_t * hfuzz, int status, fuzzer_t * fuzzer)
{
    /*
     * Resumed by delivery of SIGCONT
     */
    if (WIFCONTINUED(status)) {
        return false;
    }

    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        sancov_Analyze(hfuzz, fuzzer);
    }

    /*
     * Boring, the process just exited
     */
    if (WIFEXITED(status)) {
        LOG_D("Process (pid %d) exited normally with status %d", fuzzer->pid, WEXITSTATUS(status));
        return true;
    }

    /*
     * Shouldn't really happen, but, well..
     */
    if (!WIFSIGNALED(status)) {
        LOG_E("Process (pid %d) exited with the following status %d, please report that as a bug",
              fuzzer->pid, status);
        return true;
    }

    int termsig = WTERMSIG(status);
    LOG_D("Process (pid %d) killed by signal %d '%s'", fuzzer->pid, termsig, strsignal(termsig));
    if (!arch_sigs[termsig].important) {
        LOG_D("It's not that important signal, skipping");
        return true;
    }

    char localtmstr[PATH_MAX];
    util_getLocalTime("%F.%H:%M:%S", localtmstr, sizeof(localtmstr), time(NULL));

    char newname[PATH_MAX];

    /* If dry run mode, copy file with same name into workspace */
    if (hfuzz->origFlipRate == 0.0L && hfuzz->useVerifier) {
        snprintf(newname, sizeof(newname), "%s", fuzzer->origFileName);
    } else {
        snprintf(newname, sizeof(newname), "%s/%s.%d.%s.%s.%s",
                 hfuzz->workDir, arch_sigs[termsig].descr, fuzzer->pid, localtmstr,
                 fuzzer->origFileName, hfuzz->fileExtn);
    }

    LOG_I("Ok, that's interesting, saving the '%s' as '%s'", fuzzer->fileName, newname);

    /*
     * All crashes are marked as unique due to lack of information in POSIX arch
     */
    __sync_fetch_and_add(&hfuzz->crashesCnt, 1UL);
    __sync_fetch_and_add(&hfuzz->uniqueCrashesCnt, 1UL);

    if (files_copyFile(fuzzer->fileName, newname, NULL) == false) {
        LOG_E("Couldn't save '%s' as '%s'", fuzzer->fileName, newname);
    }
    return true;
}

pid_t arch_fork(honggfuzz_t * hfuzz UNUSED)
{
    return fork();
}

bool arch_launchChild(honggfuzz_t * hfuzz, char *fileName)
{
#define ARGS_MAX 512
    char *args[ARGS_MAX + 2];
    char argData[PATH_MAX] = { 0 };
    int x;

    for (x = 0; x < ARGS_MAX && hfuzz->cmdline[x]; x++) {
        if (!hfuzz->fuzzStdin && strcmp(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER) == 0) {
            args[x] = fileName;
        } else if (!hfuzz->fuzzStdin && strstr(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER)) {
            const char *off = strstr(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER);
            snprintf(argData, PATH_MAX, "%.*s%s", (int)(off - hfuzz->cmdline[x]), hfuzz->cmdline[x],
                     fileName);
            args[x] = argData;
        } else {
            args[x] = hfuzz->cmdline[x];
        }
    }

    args[x++] = NULL;

    LOG_D("Launching '%s' on file '%s'", args[0], fileName);

    execvp(args[0], args);
    return false;
}

void arch_reapChild(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    int status;

    for (;;) {
#ifndef __WALL
#define __WALL 0
#endif
        while (wait4(fuzzer->pid, &status, __WALL, NULL) != fuzzer->pid) ;
        LOG_D("Process (pid %d) came back with status %d", fuzzer->pid, status);

        if (arch_analyzeSignal(hfuzz, status, fuzzer)) {
            return;
        }
    }
}

bool arch_archInit(honggfuzz_t * hfuzz UNUSED)
{
    return true;
}
