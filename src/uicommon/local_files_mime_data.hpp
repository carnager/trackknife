// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QMimeData>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::ui {

// An in-process selection whose raw paths are resolved asynchronously after
// the drop. Creating or hovering a drag never walks files or queries SQLite.
class LocalFilesMimeData final : public QMimeData {
  public:
    using Completion = std::function<void(std::vector<std::string>)>;
    using Resolver = std::function<void(Completion)>;
    static QString mimeType() { return QStringLiteral("application/x-trackknife-local-files"); }

    explicit LocalFilesMimeData(Resolver resolver) : resolver_(std::move(resolver)) {
        setData(mimeType(), QByteArrayLiteral("1"));
    }
    void resolve(Completion completion) const { resolver_(std::move(completion)); }

  private:
    Resolver resolver_;
};

} // namespace trackknife::ui
