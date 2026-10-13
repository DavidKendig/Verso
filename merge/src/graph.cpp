#include "graph.h"

#include <algorithm>
#include <string>

static int FindLane(const std::vector<std::string>& lanes, const std::string& hash) {
    for (int k = 0; k < (int)lanes.size(); ++k)
        if (lanes[k] == hash) return k;
    return -1;
}

static int FreeLane(std::vector<std::string>& lanes) {
    for (int k = 0; k < (int)lanes.size(); ++k)
        if (lanes[k].empty()) return k;
    lanes.emplace_back();
    return (int)lanes.size() - 1;
}

std::vector<GraphRow> BuildGraph(const std::vector<Commit>& commits) {
    std::vector<GraphRow> rows(commits.size());
    std::vector<std::string> lanes;  // hash each lane is waiting for

    for (size_t i = 0; i < commits.size(); ++i) {
        const Commit& c = commits[i];
        GraphRow& row = rows[i];

        int col = FindLane(lanes, c.hash);
        if (col < 0) col = FreeLane(lanes);
        row.col = col;

        // Top half: every lane waiting for this commit converges on the node,
        // all other lanes pass straight through.
        std::vector<bool> passThrough(lanes.size(), false);
        for (int k = 0; k < (int)lanes.size(); ++k) {
            if (lanes[k].empty()) continue;
            if (lanes[k] == c.hash) {
                row.top.push_back({k, col});
                if (k != col) lanes[k].clear();
            } else {
                row.top.push_back({k, k});
                passThrough[k] = true;
            }
        }

        // Bottom half: the node continues into its first parent's lane and
        // branches out to any additional (merge) parents.
        lanes[col] = c.parents.empty() ? std::string() : c.parents[0];
        std::vector<int> fromNode;
        if (!c.parents.empty()) fromNode.push_back(col);
        for (size_t p = 1; p < c.parents.size(); ++p) {
            int at = FindLane(lanes, c.parents[p]);
            if (at < 0) {
                at = FreeLane(lanes);
                lanes[at] = c.parents[p];
            }
            fromNode.push_back(at);
        }
        while (!lanes.empty() && lanes.back().empty()) lanes.pop_back();

        for (int k = 0; k < (int)lanes.size(); ++k) {
            if (lanes[k].empty()) continue;
            bool fromThis = std::find(fromNode.begin(), fromNode.end(), k) != fromNode.end();
            if (fromThis) row.bottom.push_back({col, k});
            if (k < (int)passThrough.size() && passThrough[k]) row.bottom.push_back({k, k});
        }

        row.merge = c.parents.size() > 1;
        int w = std::max(col + 1, (int)lanes.size());
        for (auto& s : row.top) w = std::max({w, s.from + 1, s.to + 1});
        for (auto& s : row.bottom) w = std::max({w, s.from + 1, s.to + 1});
        row.width = w;
    }
    return rows;
}
