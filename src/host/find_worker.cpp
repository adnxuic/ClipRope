#include "host/find_worker.hpp"
#include <algorithm>
#include <map>

namespace cliprope {
FindWorker::FindWorker(std::filesystem::path root, std::function<void()> notify)
    : root_(std::move(root)), notify_(std::move(notify)), worker_([this](auto stop) { run(stop); }) {}
FindWorker::~FindWorker() {
    worker_.request_stop();
    cancel();
    wake_.notify_all();
}
void FindWorker::cancel() {
    std::lock_guard lock(mutex_);
    current_.request_stop();
    pending_.reset();
    reply_.reset();
}
void FindWorker::submit(Request request) {
    std::lock_guard lock(mutex_);
    current_.request_stop();
    current_ = std::stop_source{};
    pending_ = std::move(request);
    reply_.reset();
    wake_.notify_one();
}
std::optional<FindWorker::Reply> FindWorker::take() {
    std::lock_guard lock(mutex_);
    auto result = std::move(reply_);
    reply_.reset();
    return result;
}
void FindWorker::run(std::stop_token stop) {
    std::optional<persistence::CardSearch> reader;
    persistence::SearchResults results;
    std::optional<std::uint64_t> version;
    while (!stop.stop_requested()) {
        Request request;
        std::stop_token token;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [&] { return pending_.has_value(); })) return;
            request = std::move(*pending_);
            pending_.reset();
            token = current_.get_token();
        }
        if (request.term.empty()) {
            reader.reset();
            results = {};
            version.reset();
            continue;
        }
        Reply reply;
        try {
            if (token.stop_requested()) continue;
            if (!reader) reader.emplace(root_);
            if (version != request.version) {
                auto fresh = reader->query(request.scope, request.term, token);
                if (token.stop_requested()) continue;
                if (request.retain && fresh.total) {
                    auto& anchor = *request.retain;
                    const auto old = std::find_if(results.groups.begin(), results.groups.end(), [&](auto& g) {
                        return g.entry == anchor.entry && g.snapshot == anchor.snapshot && g.ordinal == anchor.ordinal;
                    });
                    if (old != results.groups.end()) {
                        std::map<std::pair<domain::Id, int>, const persistence::MatchGroup*> by_entry;
                        std::map<domain::Id, const persistence::MatchGroup*> source_fields;
                        for (const auto& group : fresh.groups) {
                            by_entry.emplace(std::pair{group.entry, group.ordinal}, &group);
                            if (group.field == application::SearchField::metadata) source_fields.emplace(group.entry, &group);
                        }
                        auto survivor = old;
                        request.index = 0;
                        do {
                            const persistence::MatchGroup* current{};
                            // A newly bound screenshot path is inserted before the unique source field.
                            if (survivor->field == application::SearchField::metadata) {
                                const auto found = source_fields.find(survivor->entry);
                                if (found != source_fields.end()) current = found->second;
                            } else {
                                const auto found = by_entry.find({survivor->entry, survivor->ordinal});
                                if (found != by_entry.end() && found->second->field == survivor->field) current = found->second;
                            }
                            if (current) {
                                const auto& group = *current;
                                const auto next =
                                    group.begin + (survivor == old ? std::min(anchor.occurrence, group.count - 1) : 0);
                                bool same = group.snapshot == survivor->snapshot;
                                if (!same && survivor == old && group.field == anchor.field) {
                                    // A save can replace the snapshot while preserving this exact occurrence.
                                    auto edited = reader->locate(fresh, next, token);
                                    same = edited && edited->range == anchor.range;
                                }
                                if (same) {
                                    request.index = next;
                                    break;
                                }
                            }
                            if (++survivor == results.groups.end()) survivor = results.groups.begin();
                        } while (survivor != old);
                    }
                }
                results = std::move(fresh);
                version = request.version;
            }
            reply.total = results.total;
            if (reply.total) {
                reply.index = request.index % reply.total;
                reply.hit = reader->locate(results, reply.index, token);
            }
        } catch (const persistence::StorageFailure& failure) {
            reply.error = failure.reason() == persistence::StorageProblem::unsupported_format
                ? Error::unsupported_format : Error::unavailable;
        } catch (const std::exception&) {
            reply.error = Error::unavailable;
        }
        std::lock_guard lock(mutex_);
        if (!token.stop_requested() && !stop.stop_requested()) {
            reply_ = std::move(reply);
            notify_();
        }
    }
}
} // namespace cliprope
