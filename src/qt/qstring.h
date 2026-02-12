// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_QSTRING_H
#define BITCOIN_QT_QSTRING_H

#include <tinyformat.h>

#include <string>

#include <QString>

//! Wrapper for tinyformat (strprintf) that converts to QString
template <typename... Args>
QString qstrprintf(const std::string& fmt, const Args&... args)
{
    return QString::fromStdString(tfm::format(fmt, args...));
}

#endif // BITCOIN_QT_QSTRING_H
