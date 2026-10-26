// C++/WinRT first: Qt's keyword macros (emit, signals, slots) must not be
// defined while the WinRT projection headers are parsed.
#include <windows.h>
#include <objbase.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include "services/winpdf.h"

#include <QDir>
#include <QFileInfo>

using namespace winrt;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace {

// WinRT calls block with .get(), which needs a multi-threaded apartment. The
// callers are pool threads, so each call joins the MTA for its duration.
struct MtaScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~MtaScope()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};

PdfDocument open(const QString &path)
{
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const StorageFile file = StorageFile::GetFileFromPathAsync(hstring(native.toStdWString())).get();
    return PdfDocument::LoadFromFileAsync(file).get();
}

} // namespace

namespace WinPdf {

int pageCount(const QString &path)
{
    MtaScope com;
    try {
        return int(open(path).PageCount());
    } catch (const hresult_error &) {
        return -1;
    }
}

QByteArray renderPageJpeg(const QString &path, int pageIndex, const QSize &size)
{
    MtaScope com;
    try {
        const PdfDocument document = open(path);
        if (pageIndex < 0 || uint32_t(pageIndex) >= document.PageCount())
            return {};
        const PdfPage page = document.GetPage(uint32_t(pageIndex));

        // Page size is in DIPs (1/96 inch).
        const auto pageSize = page.Size();
        double scale = 150.0 / 96.0;
        if (size.isValid() && pageSize.Width > 0 && pageSize.Height > 0)
            scale = qMin(size.width() / double(pageSize.Width), size.height() / double(pageSize.Height));
        else if (size.width() > 0 && pageSize.Width > 0)
            scale = size.width() / double(pageSize.Width);
        else if (size.height() > 0 && pageSize.Height > 0)
            scale = size.height() / double(pageSize.Height);

        // Page geometry comes from the file: cap the bitmap so a hostile
        // page size cannot make the renderer allocate gigabytes.
        constexpr double maxSide = 4096.0;
        const double longest = qMax(double(pageSize.Width), double(pageSize.Height));
        if (longest * scale > maxSide)
            scale = maxSide / longest;

        PdfPageRenderOptions options;
        options.DestinationWidth(uint32_t(qMax(1.0, pageSize.Width * scale)));
        options.DestinationHeight(uint32_t(qMax(1.0, pageSize.Height * scale)));
        options.BitmapEncoderId(Windows::Graphics::Imaging::BitmapEncoder::JpegEncoderId());

        InMemoryRandomAccessStream stream;
        page.RenderToStreamAsync(stream, options).get();
        const uint64_t size64 = stream.Size();
        if (size64 == 0 || size64 > 64u * 1024 * 1024)
            return {};
        const uint32_t bytes = uint32_t(size64);
        DataReader reader(stream.GetInputStreamAt(0));
        reader.LoadAsync(bytes).get();
        QByteArray jpeg(int(bytes), Qt::Uninitialized);
        reader.ReadBytes(array_view<uint8_t>(reinterpret_cast<uint8_t *>(jpeg.data()),
                                             reinterpret_cast<uint8_t *>(jpeg.data()) + bytes));
        return jpeg;
    } catch (const hresult_error &) {
        return {};
    }
}

} // namespace WinPdf
