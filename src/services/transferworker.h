#pragma once

// The copy/move engine behind FileOperations: GIO on Linux (gvfs URIs
// included), CopyFileExW on Windows. Both expose the same TransferItem,
// execute/cancel/pause/resume and signals.
#ifdef Q_OS_WIN
class WinTransferWorker;
using TransferWorker = WinTransferWorker;
#else
class GioTransferWorker;
using TransferWorker = GioTransferWorker;
#endif
