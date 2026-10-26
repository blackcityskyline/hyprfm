#include "services/winshell.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QWindow>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winioctl.h>

namespace {

// COM for the calling thread, released on scope exit. Worker threads call in
// here too, so each call initialises its own apartment.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};

template <typename T>
struct ComPtr {
    T *p = nullptr;
    ~ComPtr()
    {
        if (p)
            p->Release();
    }
    T **out() { return &p; }
    T *operator->() const { return p; }
};

std::wstring nativePath(const QString &path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
}

HWND activeWindow()
{
    QWindow *window = QGuiApplication::focusWindow();
    return window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

} // namespace

namespace WinShell {

bool hasRecycleBin(const QString &path)
{
    const QString root = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).left(3);
    return root.size() == 3 && root.at(1) == QLatin1Char(':')
        && GetDriveTypeW(root.toStdWString().c_str()) == DRIVE_FIXED;
}

QStringList moveToRecycleBin(const QStringList &paths)
{
    ComScope com;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.out()))))
        return paths;
    op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION
                          | FOF_SILENT | FOF_NOERRORUI);
    for (const QString &path : paths) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(nativePath(path).c_str(), nullptr,
                                                  IID_PPV_ARGS(item.out()))))
            op->DeleteItem(item.p, nullptr);
    }
    op->PerformOperations();

    QStringList left;
    for (const QString &path : paths) {
        if (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
            left.append(path);
    }
    return left;
}

QString ejectVolume(const QString &root)
{
    const QString device = QStringLiteral("\\\\.\\") + root.left(2);   // \\.\E:
    HANDLE volume = CreateFileW(device.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE)
        return QStringLiteral("the drive could not be opened");

    DWORD bytes = 0;
    QString error;
    // Lock fails while any program still has a file open on the drive.
    if (!DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        error = QStringLiteral("a program is still using it");
    } else {
        PREVENT_MEDIA_REMOVAL allow{FALSE};
        DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
        DeviceIoControl(volume, IOCTL_STORAGE_MEDIA_REMOVAL, &allow, sizeof(allow), nullptr, 0,
                        &bytes, nullptr);
        if (!DeviceIoControl(volume, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr))
            error = QStringLiteral("Windows refused to eject it");
    }
    CloseHandle(volume);
    return error;
}

QImage shellThumbnail(const QString &path, const QSize &size)
{
    ComScope com;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(nativePath(path).c_str(), nullptr,
                                           IID_PPV_ARGS(factory.out()))))
        return {};
    HBITMAP bitmap = nullptr;
    // THUMBNAILONLY: no generic file-type icon when there is no handler, so
    // the caller can fall back to its own icon.
    if (FAILED(factory->GetImage({size.width(), size.height()}, SIIGBF_THUMBNAILONLY, &bitmap)))
        return {};
    QImage image = QImage::fromHBITMAP(bitmap);
    DeleteObject(bitmap);
    return image;
}

void openWithDialog(const QString &path)
{
    const std::wstring file = nativePath(path);
    OPENASINFO info{};
    info.pcszFile = file.c_str();
    info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC;
    SHOpenWithDialog(activeWindow(), &info);
}

QString setWallpaper(const QString &path)
{
    std::wstring file = nativePath(path);
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, file.data(),
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        return QStringLiteral("Windows refused the image (error %1)").arg(GetLastError());
    return {};
}

QString pickFolder(const QString &title, const QString &startFolder, const QString &okLabel)
{
    ComScope com;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.out()))))
        return {};
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.toStdWString().c_str());
    if (!okLabel.isEmpty())
        dialog->SetOkButtonLabel(okLabel.toStdWString().c_str());
    ComPtr<IShellItem> start;
    if (SUCCEEDED(SHCreateItemFromParsingName(nativePath(startFolder).c_str(), nullptr,
                                              IID_PPV_ARGS(start.out()))))
        dialog->SetFolder(start.p);
    if (FAILED(dialog->Show(activeWindow())))
        return {};   // cancelled
    ComPtr<IShellItem> result;
    if (FAILED(dialog->GetResult(result.out())))
        return {};
    PWSTR chosen = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &chosen)))
        return {};
    const QString folder = QDir::fromNativeSeparators(QString::fromWCharArray(chosen));
    CoTaskMemFree(chosen);
    return folder;
}

} // namespace WinShell
