/*
 * Copyright (C) 2014 Aaron Seigo <aseigo@kde.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3, or any
 * later version accepted by the membership of KDE e.V. (or its
 * successor approved by the membership of KDE e.V.), which shall
 * act as a proxy defined in Section 6 of version 3 of the license.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "storage.h"

#include <iostream>

#include <QAtomicInt>
#include <QDebug>
#include <QDir>
#include <QReadWriteLock>
#include <QString>
#include <QTime>

extern "C" {
    #include "unqlite/unqlite.h"
}

namespace Akonadi2
{

static const char *s_unqliteDir = "/unqlite/";

class Storage::Private
{
public:
    Private(const QString &s, const QString &name, AccessMode m, bool allowDuplicates);
    ~Private();

    void reportDbError(const char *functionName);
    void reportDbError(const char *functionName, int errorCode,
                       const std::function<void(const Storage::Error &error)> &errorHandler);

    QString storageRoot;
    QString name;
    AccessMode mode;

    unqlite *db;
    bool allowDuplicates;
    bool inTransaction;
};

Storage::Private::Private(const QString &s, const QString &n, AccessMode m, bool duplicates)
    : storageRoot(s),
      name(n),
      mode(m),
      db(0),
      allowDuplicates(duplicates), //FIXME: currently does nothing ... should do what it says
      inTransaction(false)
{
    const QString fullPath(storageRoot + s_unqliteDir + name);
    QDir dir;
    dir.mkpath(storageRoot + s_unqliteDir);

    //create file
    int openFlags = UNQLITE_OPEN_CREATE;
    if (mode == ReadOnly) {
        openFlags |= UNQLITE_OPEN_READONLY | UNQLITE_OPEN_MMAP;
    } else {
        openFlags |= UNQLITE_OPEN_READWRITE;
    }

    int rc = unqlite_open(&db, fullPath.toStdString().data(), openFlags);

    if (rc != UNQLITE_OK) {
        reportDbError("unqlite_open");
    }
}

Storage::Private::~Private()
{
    unqlite_close(db);
}

void Storage::Private::reportDbError(const char *functionName)
{
    std::cerr << "ERROR: " << functionName;
    if (db) {
        const char *errorMessage;
        int length;
        /* Something goes wrong, extract database error log */
        unqlite_config(db, UNQLITE_CONFIG_ERR_LOG, &errorMessage, &length);
        if (length > 0) {
            std::cerr << ": " << errorMessage;
        }
    }
    std::cerr << std::endl;
}

void Storage::Private::reportDbError(const char *functionName, int errorCode,
                                     const std::function<void(const Storage::Error &error)> &errorHandler)
{
    if (db) {
        const char *errorMessage;
        int length;
        /* Something goes wrong, extract database error log */
        unqlite_config(db, UNQLITE_CONFIG_ERR_LOG, &errorMessage, &length);
        if (length > 0) {
            Error error(name.toStdString(), errorCode, errorMessage);
            errorHandler(error);
            return;
        }
    }

    Error error(name.toStdString(), errorCode, functionName);
    errorHandler(error);
}

Storage::Storage(const QString &storageRoot, const QString &name, AccessMode mode, bool allowDuplicates)
    : d(new Private(storageRoot, name, mode, allowDuplicates))
{
}

Storage::~Storage()
{
    if (d->inTransaction) {
        abortTransaction();
    }

    delete d;
}

bool Storage::isInTransaction() const
{
    return d->inTransaction;
}

bool Storage::startTransaction(AccessMode type)
{
    if (!d->db) {
        return false;
    }

    if (d->inTransaction) {
        return true;
    }

    d->inTransaction = unqlite_begin(d->db) == UNQLITE_OK;

    if (!d->inTransaction) {
        d->reportDbError("unqlite_begin");
    }

    return d->inTransaction;
}

bool Storage::commitTransaction()
{
    if (!d->db) {
        return false;
    }

    if (!d->inTransaction) {
        return true;
    }

    int rc = unqlite_commit(d->db);
    d->inTransaction = false;

    if (rc != UNQLITE_OK) {
        d->reportDbError("unqlite_commit");
    }

    return rc == UNQLITE_OK;
}

void Storage::abortTransaction()
{
    if (!d->db || !d->inTransaction) {
        return;
    }

    unqlite_rollback(d->db);
    d->inTransaction = false;
}

bool Storage::write(const void *key, size_t keySize, const void *value, size_t valueSize)
{
    if (!d->db) {
        return false;
    }

    int rc = unqlite_kv_store(d->db, key, keySize, value, valueSize);

    if (rc != UNQLITE_OK) {
        d->reportDbError("unqlite_kv_store");
    }

    return !rc;
}

bool Storage::write(const std::string &sKey, const std::string &sValue)
{
    return write(sKey.data(), sKey.size(), sValue.data(), sKey.size());
}

void Storage::read(const std::string &sKey,
                   const std::function<bool(const std::string &value)> &resultHandler,
                   const std::function<void(const Storage::Error &error)> &errorHandler)
{
    read(sKey,
         [&](void *ptr, int size) -> bool {
            if (ptr) {
                const std::string resultValue(static_cast<char*>(ptr), size);
                return resultHandler(resultValue);
            }

            return true;
         }, errorHandler);
}

void Storage::read(const std::string &sKey,
                   const std::function<bool(void *ptr, int size)> &resultHandler,
                   const std::function<void(const Storage::Error &error)> &errorHandler)
{
    scan(sKey.data(), sKey.size(), [resultHandler](void *keyPtr, int keySize, void *valuePtr, int valueSize) {
        return resultHandler(valuePtr, valueSize);
    }, errorHandler);
}

void Storage::remove(const void *keyData, uint keySize)
{
    remove(keyData, keySize, basicErrorHandler());
}

void Storage::remove(const void *keyData, uint keySize,
                     const std::function<void(const Storage::Error &error)> &errorHandler)
{
    if (!d->db) {
        Error error(d->name.toStdString(), -1, "Not open");
        errorHandler(error);
        return;
    }

    unqlite_kv_delete(d->db, keyData, keySize);
}


void fetchCursorData(unqlite_kv_cursor *cursor,
                     void **keyBuffer, int *keyBufferLength, void **dataBuffer, unqlite_int64 *dataBufferLength,
                     const std::function<bool(void *keyPtr, int keySize, void *valuePtr, int valueSize)> &resultHandler)
{
    int keyLength = 0;
    unqlite_int64 dataLength = 0;
    // now fetch the data sizes
    if (unqlite_kv_cursor_key(cursor, nullptr, &keyLength) == UNQLITE_OK &&
        unqlite_kv_cursor_data(cursor, nullptr, &dataLength) == UNQLITE_OK) {
        if (keyLength > *keyBufferLength) {
            *keyBuffer = realloc(*keyBuffer, keyLength);
            *keyBufferLength = keyLength;
        }

        if (dataLength > *dataBufferLength) {
            *dataBuffer = realloc(*dataBuffer, dataLength);
            *dataBufferLength = dataLength;
        }

        if (unqlite_kv_cursor_key(cursor, *keyBuffer, &keyLength) == UNQLITE_OK &&
            unqlite_kv_cursor_data(cursor, *dataBuffer, &dataLength) == UNQLITE_OK) {
            resultHandler(*keyBuffer, keyLength, *dataBuffer, dataLength);
        }
    }
}

void Storage::scan(const char *keyData, uint keySize,
                   const std::function<bool(void *keyPtr, int keySize, void *valuePtr, int valueSize)> &resultHandler,
                   const std::function<void(const Storage::Error &error)> &errorHandler)
{
    if (!d->db) {
        Error error(d->name.toStdString(), -1, "Not open");
        errorHandler(error);
        return;
    }

    unqlite_kv_cursor *cursor;

    int rc = unqlite_kv_cursor_init(d->db, &cursor);
    if (rc != UNQLITE_OK) {
        d->reportDbError("unqlite_kv_cursor_init", rc, errorHandler);
        return;
    }

    void *keyBuffer = nullptr;
    int keyBufferLength = 0;
    void *dataBuffer = nullptr;
    //FIXME: 64bit ints, but feeding int lenghts to the callbacks. can result in truncation
    unqlite_int64 dataBufferLength = 0;
    if (!keyData || keySize == 0) {
        for (unqlite_kv_cursor_first_entry(cursor); unqlite_kv_cursor_valid_entry(cursor); unqlite_kv_cursor_next_entry(cursor)) {
            fetchCursorData(cursor, &keyBuffer, &keyBufferLength, &dataBuffer, &dataBufferLength, resultHandler);
        }
    } else {
        rc = unqlite_kv_cursor_seek(cursor, keyData, keySize, UNQLITE_CURSOR_MATCH_EXACT);
        if (rc == UNQLITE_OK) {
            fetchCursorData(cursor, &keyBuffer, &keyBufferLength, &dataBuffer, &dataBufferLength, resultHandler);
        } else {
            std::cout << "couldn't find value " << std::string(keyData, keySize) << std::endl;
        }

    }

    free(keyBuffer);
    free(dataBuffer);
    unqlite_kv_cursor_release(d->db, cursor);
}

qint64 Storage::diskUsage() const
{
    QFileInfo info(d->storageRoot + s_unqliteDir + d->name);
    return info.size();
}

bool Storage::exists() const
{
    return d->db != 0;
}

void Storage::removeFromDisk() const
{
    QFile::remove(d->storageRoot + s_unqliteDir + d->name);
}

} // namespace Akonadi2
