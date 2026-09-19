// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

declare class FileReaderSync {
  readAsArrayBuffer(blob: Blob): ArrayBuffer;
}

interface FileSystemSyncAccessHandle {
  write(buffer: ArrayBufferView | ArrayBuffer, options?: { at?: number }): number;
  truncate(size: number): void;
  flush(): void;
  close(): void;
}

interface FileSystemFileHandle {
  createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle>;
}
