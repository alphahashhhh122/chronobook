#pragma once

#include "journal/FillJournal.h"

namespace chronobook {

class JournalReplay {
public:
    static std::vector<FillRecord> load(const std::string& path) {
        return FillJournal::replay(path);
    }
};

} // namespace chronobook
