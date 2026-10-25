#pragma once

#include <QUrl>

// Whether a location string is a URI (sftp://, trash:///, file://) rather than
// a plain path. QUrl reads "C:/Users" as scheme "c", so a one-letter scheme is
// a Windows drive, not a URI.
inline bool hasUriScheme(const QUrl &url)
{
    return url.scheme().size() > 1;
}
