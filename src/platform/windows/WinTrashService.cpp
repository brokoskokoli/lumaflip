// SPDX-License-Identifier: GPL-3.0-or-later
// Recycle Bin (F-FILE-01) on Windows.
//
// Moving: IFileOperation::DeleteItem with FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO (the same
// operation as Explorer's "Delete", undoable there as well). A progress sink receives the
// item the Recycle Bin created ("<volume>\$Recycle.Bin\<SID>\$R<id>.jpg"); that path is the
// "location inside the trash". If Windows cannot recycle the file (a volume without Recycle
// Bin, a network share, a file larger than the bin), FOF_WANTNUKEWARNING makes it ask the
// user instead of deleting permanently without a word.
//
// Restoring: the "$R" item is moved back (MoveFileExW: never over an existing file) and the
// matching "$I" record (original path and deletion date that the Recycle Bin folder shows)
// is removed, so the bin does not list a ghost entry. This is what Explorer's "Restore"
// does as well.
//
// IFileOperation needs a single-threaded apartment: the operation runs on a short-lived
// STA thread (callers are worker threads without COM).

#include "core/Platform.h"
#include "platform/windows/WinCom.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace lf {

namespace {

using Microsoft::WRL::ComPtr;

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::TrashService)
};

// Collects the path of the item the Recycle Bin created - or that there is none: the shell
// deleted the file for good (it did not fit into the Recycle Bin, or the drive has none).
class DeleteSink final : public IFileOperationProgressSink {
public:
    QString recycled;
    bool permanentlyDeleted = false;
    HRESULT result = S_OK;

    // IUnknown (stack object: no reference counting)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT hr,
                                             IShellItem* newlyCreated) override {
        result = hr;
        // "If the item was fully deleted, this value is NULL" (IFileOperationProgressSink).
        permanentlyDeleted = SUCCEEDED(hr) && !newlyCreated;
        if (SUCCEEDED(hr) && newlyCreated) {
            LPWSTR name = nullptr;
            if (SUCCEEDED(newlyCreated->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                recycled = QDir::fromNativeSeparators(QString::fromWCharArray(name));
                CoTaskMemFree(name);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT,
                                           IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT,
                                           IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT,
                                          IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};

// "$R<id>.ext" -> "$I<id>.ext" next to it (the Recycle Bin's record of the item).
QString recordOf(const QString& recycledPath) {
    const QFileInfo fi(recycledPath);
    const QString name = fi.fileName();
    if (!name.startsWith(QLatin1String("$R")))
        return {};
    return fi.dir().filePath(QStringLiteral("$I") + name.mid(2));
}

class WinTrashService final : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        QString recycled;
        HRESULT hr = E_FAIL;
        bool removed = false;
        bool permanentlyDeleted = false;
        win::runInSta([&] {
            ComPtr<IFileOperation> op;
            ComPtr<IShellItem> item;
            hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
            // FOF_WANTNUKEWARNING: a file that cannot go to the Recycle Bin is never deleted
            // without Windows asking first (it overrides FOF_NOCONFIRMATION for that case).
            if (SUCCEEDED(hr))
                hr = op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION |
                                           FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING |
                                           FOFX_EARLYFAILURE);
            if (SUCCEEDED(hr))
                hr = SHCreateItemFromParsingName(win::wstr(native), nullptr, IID_PPV_ARGS(&item));
            DeleteSink sink;
            if (SUCCEEDED(hr))
                hr = op->DeleteItem(item.Get(), &sink);
            if (SUCCEEDED(hr))
                hr = op->PerformOperations();
            BOOL aborted = FALSE;
            if (SUCCEEDED(hr) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted)
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (SUCCEEDED(hr) && FAILED(sink.result))
                hr = sink.result;
            recycled = sink.recycled;
            permanentlyDeleted = sink.permanentlyDeleted;
            removed = SUCCEEDED(hr);
        });
        if (removed && permanentlyDeleted && !QFileInfo::exists(path)) {
            // The user confirmed Windows' "delete permanently?" question: gone, nothing to undo.
            // Reported as a failure so that no undo step is offered for it.
            if (error)
                *error = Tr::tr("„%1“ passte nicht in den Papierkorb und wurde endgültig gelöscht.")
                             .arg(QFileInfo(path).fileName());
            return std::nullopt;
        }
        if (!removed || QFileInfo::exists(path)) {
            if (error)
                *error = Tr::tr("„%1“ konnte nicht in den Papierkorb gelegt werden: %2")
                             .arg(QFileInfo(path).fileName(), win::errorText(DWORD(hr)));
            return std::nullopt;
        }
        return recycled; // empty: recycled, but Windows did not say where
    }

    bool restoreFromTrash(const QString& trashedPath, const QString& originalPath, QString* error) override {
        const QFileInfo orig(originalPath);
        if (orig.exists() || orig.isSymLink()) {
            if (error)
                *error = Tr::tr("„%1“ existiert bereits und wird nicht überschrieben.").arg(orig.fileName());
            return false;
        }
        if (trashedPath.isEmpty() || !QFileInfo::exists(trashedPath)) {
            if (error)
                *error = Tr::tr("„%1“ ist nicht mehr im Papierkorb.").arg(orig.fileName());
            return false;
        }
        QDir().mkpath(orig.absolutePath());
        const QString from = QDir::toNativeSeparators(trashedPath);
        const QString to = QDir::toNativeSeparators(orig.absoluteFilePath());
        if (!MoveFileExW(win::wstr(from), win::wstr(to), MOVEFILE_COPY_ALLOWED)) {
            if (error)
                *error = Tr::tr("„%1“ konnte nicht wiederhergestellt werden: %2")
                             .arg(orig.fileName(), win::errorText(GetLastError()));
            return false;
        }
        const QString record = recordOf(trashedPath);
        if (!record.isEmpty())
            QFile::remove(record);
        // Explorer windows showing the Recycle Bin or the folder update themselves.
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, win::wstr(to), nullptr);
        return true;
    }
};

} // namespace

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<WinTrashService>();
}

} // namespace lf
