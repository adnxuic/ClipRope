#pragma once
#include "persistence/history_store.hpp"

namespace order_fixture {
inline cliprope::domain::Entry seed(cliprope::persistence::HistoryStore& store, unsigned size,
                                    std::string_view rope, const cliprope::domain::Snapshot& snapshot) {
    if (rope != cliprope::domain::rebin_id) store.set_capacity(rope, static_cast<int>(size));
    const auto head = store.entry(store.admit(rope, snapshot));
    if (size > 1) {
        cliprope::persistence::Statement add(store.database().handle(),
            "WITH RECURSIVE rows(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM rows WHERE n+1<?) "
            "INSERT INTO entries(id,snapshot_id,state,rope_id,sort_key) "
            "SELECT printf('benchmark-row-%06d',n),?,'Hanging',?,n FROM rows");
        add.bind(1, static_cast<int>(size));
        add.bind(2, head.snapshot_id);
        add.bind(3, rope);
        add.run();
    }
    return head;
}
}
