// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows/WinDecoderSandbox.h"

#include "core/CommandLine.h"
#include "core/DecoderIpc.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcessEnvironment>

#include <vector>

#include <windows.h>

namespace lf::win {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::DecoderHelper)
};

QString lastError() {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text = n && buffer ? QString::fromWCharArray(buffer, int(n)).trimmed() : QString::number(code);
    if (buffer)
        LocalFree(buffer);
    return text;
}

// A primary token for the helper: the app's token without privileges, at low integrity.
HANDLE lowIntegrityToken(QString* why) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT,
                          &token)) {
        *why = QStringLiteral("OpenProcessToken: ") + lastError();
        return nullptr;
    }
    HANDLE restricted = nullptr;
    const BOOL ok =
        CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted);
    CloseHandle(token);
    if (!ok) {
        *why = QStringLiteral("CreateRestrictedToken: ") + lastError();
        return nullptr;
    }
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID low = nullptr;
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low)) {
        CloseHandle(restricted);
        *why = QStringLiteral("AllocateAndInitializeSid: ") + lastError();
        return nullptr;
    }
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = low;
    const BOOL set =
        SetTokenInformation(restricted, TokenIntegrityLevel, &label, DWORD(sizeof label + GetLengthSid(low)));
    FreeSid(low);
    if (!set) {
        CloseHandle(restricted);
        *why = QStringLiteral("SetTokenInformation: ") + lastError();
        return nullptr;
    }
    return restricted;
}

// Diagnostics: LF_DECODER_HELPER_SANDBOX_OFF=token,job,ui,mitigations,childpolicy switches parts
// of the sandbox off (the helper then reports sandboxed = false where that matters).
bool sandboxPartOff(const char* part) {
    return qEnvironmentVariable("LF_DECODER_HELPER_SANDBOX_OFF")
        .split(u',', Qt::SkipEmptyParts)
        .contains(QLatin1String(part));
}

HANDLE helperJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                             JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        (!sandboxPartOff("ui") &&
         !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

std::vector<wchar_t> environmentBlock(const QStringList& extra) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString& e : extra) {
        const qsizetype eq = e.indexOf(u'=');
        if (eq > 0)
            env.insert(e.left(eq), e.mid(eq + 1));
    }
    std::vector<wchar_t> block;
    for (const QString& key : env.keys()) {
        const QString entry = key + u'=' + env.value(key);
        const std::wstring w = entry.toStdWString();
        block.insert(block.end(), w.begin(), w.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1)
        block.push_back(L'\0');
    return block;
}

HANDLE inheritableStderr() {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE dup = nullptr;
    if (err && err != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return dup;
    dup = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                      nullptr);
    return dup == INVALID_HANDLE_VALUE ? nullptr : dup;
}

} // namespace

bool startDecoderHelper(const QString& exe, const QStringList& args, const QStringList& extraEnvironment,
                        HelperProcess* out, QString* error) {
    auto fail = [&](const QString& why) {
        if (error)
            *error = Tr::tr("Der Decoder-Hilfsprozess konnte nicht gestartet werden (%1).").arg(why);
        return false;
    };
    int childIn = -1, appWrite = -1, appRead = -1, childOut = -1;
    if (!ipc::createPipe(&childIn, &appWrite, false, true, true, false))
        return fail(QStringLiteral("pipe: ") + lastError());
    if (!ipc::createPipe(&appRead, &childOut, true, false, false, true)) {
        const QString why = lastError();
        ipc::closeHandle(childIn);
        ipc::closeHandle(appWrite);
        return fail(QStringLiteral("pipe: ") + why);
    }
    HANDLE childErr = inheritableStderr();
    auto closeChildEnds = [&] {
        ipc::closeHandle(childIn);
        ipc::closeHandle(childOut);
        if (childErr)
            CloseHandle(childErr);
    };
    auto closeAll = [&] {
        closeChildEnds();
        ipc::closeHandle(appWrite);
        ipc::closeHandle(appRead);
    };

    HANDLE job = helperJob();
    if (!job) {
        const QString why = lastError();
        closeAll();
        return fail(QStringLiteral("job: ") + why);
    }
    QString tokenProblem;
    HANDLE token = sandboxPartOff("token") ? nullptr : lowIntegrityToken(&tokenProblem);
    if (!token && !tokenProblem.isEmpty())
        qWarning("lumaflip: decoder helper without low integrity token: %s", qPrintable(tokenProblem));

    // Inherited: exactly the helper's stdin, stdout and stderr.
    HANDLE inherit[3] = {LongToHandle(childIn), LongToHandle(childOut), childErr};
    const DWORD inheritCount = childErr ? 3 : 2;
    HANDLE jobs[1] = {job};
    DWORD64 mitigations = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON;
    DWORD childPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    const bool useJob = !sandboxPartOff("job");
    const bool useMitigations = !sandboxPartOff("mitigations");
    const bool useChildPolicy = !sandboxPartOff("childpolicy");
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 4, 0, &attrSize);
    std::vector<char> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    bool attrsOk =
        InitializeProcThreadAttributeList(attrs, 4, 0, &attrSize) &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                  inheritCount * sizeof(HANDLE), nullptr, nullptr) &&
        (!useJob || UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof jobs,
                                              nullptr, nullptr)) &&
        (!useMitigations || UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                                                      &mitigations, sizeof mitigations, nullptr, nullptr)) &&
        (!useChildPolicy || UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                                      &childPolicy, sizeof childPolicy, nullptr, nullptr));
    if (!attrsOk) {
        const QString why = lastError();
        if (token)
            CloseHandle(token);
        CloseHandle(job);
        closeAll();
        return fail(QStringLiteral("attributes: ") + why);
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = LongToHandle(childIn);
    si.StartupInfo.hStdOutput = LongToHandle(childOut);
    si.StartupInfo.hStdError = childErr;
    si.lpAttributeList = attrs;
    const QString program = QDir::toNativeSeparators(exe);
    QString line = cmdline::build(program, args);
    std::vector<wchar_t> lineBuffer(size_t(line.size()) + 1, L'\0');
    line.toWCharArray(lineBuffer.data());
    std::vector<wchar_t> env = environmentBlock(extraEnvironment);
    // DETACHED_PROCESS: no console (and no console host process, which the child process
    // policy would refuse - the helper then dies at start-up); its std handles are the pipes.
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS;
    PROCESS_INFORMATION pi{};
    const std::wstring app = program.toStdWString();
    BOOL started = FALSE;
    if (token) {
        started = CreateProcessAsUserW(token, app.c_str(), lineBuffer.data(), nullptr, nullptr, TRUE, flags,
                                       env.data(), nullptr, &si.StartupInfo, &pi);
        if (!started)
            qWarning("lumaflip: starting the decoder helper at low integrity failed: %s",
                     qPrintable(lastError()));
    }
    if (!started) // without the low integrity token (the helper reports it; the app may refuse it)
        started = CreateProcessW(app.c_str(), lineBuffer.data(), nullptr, nullptr, TRUE, flags, env.data(),
                                 nullptr, &si.StartupInfo, &pi);
    const QString startError = started ? QString() : lastError();
    DeleteProcThreadAttributeList(attrs);
    if (token)
        CloseHandle(token);
    closeChildEnds();
    if (!started) {
        CloseHandle(job);
        ipc::closeHandle(appWrite);
        ipc::closeHandle(appRead);
        return fail(startError);
    }
    CloseHandle(pi.hThread);
    out->pid = qint64(pi.dwProcessId);
    out->process = pi.hProcess;
    out->job = job;
    out->toChild = appWrite;
    out->fromChild = appRead;
    return true;
}

void killDecoderHelper(HelperProcess* p) {
    ipc::closeHandle(p->toChild);
    ipc::closeHandle(p->fromChild);
    DWORD code = 0;
    if (p->process && WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(p->process, &code))
        qWarning("lumaflip: decoder helper %lld had exited with 0x%08lx", p->pid,
                 static_cast<unsigned long>(code));
    if (p->process) {
        TerminateProcess(p->process, 1);
        WaitForSingleObject(p->process, 5000);
        CloseHandle(p->process);
    }
    if (p->job)
        CloseHandle(p->job); // KILL_ON_JOB_CLOSE: nothing of the helper survives
    *p = HelperProcess{};
}

bool verifyDecoderSandbox(QString* error) {
    // From here on no code may be generated at run time (decoders do not need it).
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    dynamicCode.ProhibitDynamicCode = 1;
    SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicCode, sizeof dynamicCode);

    DWORD rid = SECURITY_MANDATORY_HIGH_RID;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        std::vector<char> buffer(size);
        if (size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
            rid = *GetSidSubAuthority(sid, DWORD(*GetSidSubAuthorityCount(sid) - 1));
        }
        CloseHandle(token);
    }
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    if (rid > SECURITY_MANDATORY_LOW_RID || !inJob) {
        if (error)
            *error = QStringLiteral("integrity level 0x%1, in job: %2")
                         .arg(rid, 0, 16)
                         .arg(inJob ? QStringLiteral("yes") : QStringLiteral("no"));
        return false;
    }
    return true;
}

bool processAlive(qint64 pid) {
    if (pid <= 0)
        return false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

} // namespace lf::win
