#pragma once
#include <vector>

#include "git.h"

// One line segment of the commit graph inside a single row. Lanes are columns;
// `top` segments run from the row's top edge to its centre, `bottom` segments
// from the centre to the bottom edge.
struct GraphSeg {
    int from, to;
};

struct GraphRow {
    int col = 0;  // lane holding this commit's node
    std::vector<GraphSeg> top, bottom;
    int width = 1;  // number of lanes used in this row
    bool merge = false;
};

// Assigns every commit (in display order, newest first) to a lane.
std::vector<GraphRow> BuildGraph(const std::vector<Commit>& commits);
