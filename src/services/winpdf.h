#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

// PDFs through Windows' own renderer (Windows.Data.Pdf, Windows 10+), so the
// Windows build needs neither poppler nor a bundled PDF engine. Blocking;
// call from worker threads.
namespace WinPdf {

// Page count, or -1 when the file cannot be opened (damaged, encrypted).
int pageCount(const QString &path);

// One page (0-based) as JPEG, fitted inside `size`; an invalid size renders
// at 150 dpi. Empty on failure.
QByteArray renderPageJpeg(const QString &path, int page, const QSize &size);

} // namespace WinPdf
