#include "Assembler.hpp"
#include "orderPairs.hpp"
using namespace shasta;

// Boost libraries.
#include <boost/pending/disjoint_sets.hpp>



void Assembler::createConflictReadGraph(
    uint64_t threadCount,
    uint32_t maxOffsetSigma,
    uint32_t maxTrim,
    uint32_t maxSkip)
{
    cout << timestamp << "createConflictReadGraph begins." << endl;

    // Check that we have what we need.
    // The code as written only supports the directed read graph.
    SHASTA_ASSERT(directedReadGraph.edges.isOpen);
    SHASTA_ASSERT(directedReadGraph.edgesBySource.isOpen());
    SHASTA_ASSERT(directedReadGraph.edgesByTarget.isOpen());
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();

    // Adjust the number of threads.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Store criteria for the induced alignment so all threads can see them.
    createConflictReadGraphData.inducedAlignmentCriteria.maxOffsetSigma = maxOffsetSigma;
    createConflictReadGraphData.inducedAlignmentCriteria.maxTrim = maxTrim;
    createConflictReadGraphData.inducedAlignmentCriteria.maxSkip = maxSkip;

    // Initialize the conflict read graph.
    conflictReadGraph.createNew(largeDataName("ConflictReadGraph"), largeDataPageSize);
    conflictReadGraph.createVertices(readCount());

    // Add edges.
    conflictReadGraph.edges.reserve(10 * readCount());
    setupLoadBalancing(readCount(), 1);
    runThreads(&Assembler::createConflictReadGraphThreadFunction, threadCount);
    conflictReadGraph.edges.unreserve();
    conflictReadGraph.computeConnectivity();


    cout << "The conflict read graph has " <<
        conflictReadGraph.vertices.size() << " vertices and " <<
        conflictReadGraph.edges.size() << " edges" << endl;
    cout << timestamp << "createConflictReadGraph ends." << endl;
}



void Assembler::accessConflictReadGraph()
{
    conflictReadGraph.accessExistingReadWrite(largeDataName("ConflictReadGraph"));

}



void Assembler::createConflictReadGraphThreadFunction(size_t threadId)
{
    const InducedAlignmentCriteria inducedAlignmentCriteria =
        createConflictReadGraphData.inducedAlignmentCriteria;

    // Work areas for addConflictGraphEdges.
    vector<OrientedReadId> conflictCandidates;
    vector<OrientedReadId> conflictingOrientedReads;
    vector<InducedAlignment> inducedAlignments;
    vector<bool> work0;
    vector<bool> work1;

    // Loop over batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over reads in this batch.
        for(ReadId readId=ReadId(begin); readId!=ReadId(end); ++readId) {
            // cout << "Working on read " << readId << endl;
            addConflictGraphEdges(
                readId,
                inducedAlignmentCriteria,
                conflictCandidates,
                conflictingOrientedReads,
                inducedAlignments,
                work0,
                work1);
        }
    }

}



// This creates edges of the conflict read graph where
// the lower number read is readId0.
// It add the edges to the conflict read graph directly, under mutex protection.
// This should not create significant contention as adding edges to the
// graph is most of the times much faster than computing them.
void Assembler::addConflictGraphEdges(
    ReadId readId0,
    const InducedAlignmentCriteria& inducedAlignmentCriteria,

    // Work areas.
    vector<OrientedReadId>& conflictCandidates,
    vector<OrientedReadId>& conflictingOrientedReads,
    vector<InducedAlignment>& inducedAlignments,
    vector<bool>& work0,
    vector<bool>& work1)
{

    // Put this read on strand 0.
    // When adding edges to the conflict read graph, we will make sure
    // to also add the reverse complemented edge.
    const OrientedReadId orientedReadId0(readId0, 0);



    // Find conflict candidates for orientedReadId0.
    // These are OrientedReadId's that share at least one marker graph vertex
    // with orientedReadId0.
    // To do this, we loop over all markers of orientedReadId0.
    conflictCandidates.clear();
    const MarkerId firstMarkerId = markers.begin(orientedReadId0.getValue()) - markers.begin();
    const uint32_t markerCount = uint32_t(markers.size(orientedReadId0.getValue()));
    for(uint32_t ordinal=0; ordinal<markerCount; ordinal++) {
        const MarkerId markerId0 = firstMarkerId + ordinal;

        // Find the vertex that this marker is on.
        const MarkerGraph::CompressedVertexId compressedVertexId =
            markerGraph.vertexTable[markerId0];

        // If this marker is not on a marker graph vertex, skip.
        if(compressedVertexId == MarkerGraph::invalidCompressedVertexId) {
            continue;
        }

        // Loop over all markers on this vertex.
        const span<MarkerId> vertexMarkers =
            markerGraph.vertices[compressedVertexId];
        for(const MarkerId markerId1: vertexMarkers) {

            // Skip the marker that we started from.
            if(markerId1 == markerId0) {
                continue;
            }

            // Find the oriented read on this marker.
            OrientedReadId orientedReadId1;
            uint32_t ordinal1;
            tie(orientedReadId1, ordinal1) = findMarkerId(markerId1);

            // Only consider markers with readId1>readId0.
            if(orientedReadId1.getReadId() <= readId0) {
                continue;
            }

            // Add this oriented read to our conflict candidates.
           conflictCandidates.push_back(orientedReadId1);
        }
    }
    deduplicate(conflictCandidates);



    // Remove conflict candidates that correspond to an edge of the read graph.
    // For those we already have a good alignment.
    auto itA = conflictCandidates.begin();
    auto itB = itA;
    for(; itA!=conflictCandidates.end(); ++itA) {
        const OrientedReadId orientedReadId1 = *itA;
        const DirectedReadGraph::VertexId v0 = orientedReadId0.getValue();
        const DirectedReadGraph::VertexId v1 = orientedReadId1.getValue();
        const bool forwardExists = directedReadGraph.findEdge(v0, v1)
            != DirectedReadGraph::invalidEdgeId;
        const bool backwardExists = directedReadGraph.findEdge(v1, v0)
            != DirectedReadGraph::invalidEdgeId;

        if(forwardExists or backwardExists) {
            continue;
        } else {
            *itB++ = orientedReadId1;
        }
    }
    conflictCandidates.resize(itB - conflictCandidates.begin());



    // Compute induced alignments between orientedReadId0 and these conflict candidates.
    computeInducedAlignments(
        orientedReadId0,
        conflictCandidates,
        inducedAlignments
    );
    SHASTA_ASSERT(inducedAlignments.size() == conflictCandidates.size());

    // Find which of the induced alignments are bad.
    conflictingOrientedReads.clear();
    const uint32_t markerCount0 = uint32_t(markers.size(orientedReadId0.getValue()));
    for(uint64_t i=0;i<inducedAlignments.size(); i++) {
        const OrientedReadId orientedReadId1 = conflictCandidates[i];
        const uint32_t markerCount1 = uint32_t(markers.size(orientedReadId1.getValue()));
        // cout << "Checking induced alignment of " << orientedReadId0 << " " << orientedReadId1 << endl;

        if(not inducedAlignments[i].evaluate(
            markerCount0,
            markerCount1,
            inducedAlignmentCriteria)) {
            conflictingOrientedReads.push_back(orientedReadId1);
        }
#if 0
        // This also takes into account the presence or absence of marker graph vertices.
        if(not evaluateInducedAlignment(
            orientedReadId0,
            orientedReadId1,
            inducedAlignments[i],
            inducedAlignmentCriteria,
            work0,
            work1)) {
            conflictingOrientedReads.push_back(orientedReadId1);
        }
#endif
    }
    // cout << "Counts: " << conflictCandidates.size () << " " << conflictingOrientedReads.size() << endl;


    // Add edges to the conflict graph.
    {
        // Find the vertices corresponding to the first read.
        using VertexId = ConflictReadGraph::VertexId;
        const VertexId vertexId0 =
            ConflictReadGraph::getVertexId(orientedReadId0);
        OrientedReadId orientedReadId0ReverseComplement = orientedReadId0;
        orientedReadId0ReverseComplement.flipStrand();
        const VertexId vertexId0ReverseComplement =
            ConflictReadGraph::getVertexId(orientedReadId0ReverseComplement);

        std::lock_guard<std::mutex> lock(mutex);
        for(const OrientedReadId orientedReadId1: conflictingOrientedReads) {

            // Find the vertices corresponding to the second read.
            const VertexId vertexId1 =
                ConflictReadGraph::getVertexId(orientedReadId1);
            OrientedReadId orientedReadId1ReverseComplement = orientedReadId1;
            orientedReadId1ReverseComplement.flipStrand();
            const VertexId vertexId1ReverseComplement =
                ConflictReadGraph::getVertexId(orientedReadId1ReverseComplement);

            // Add the edges.
            conflictReadGraph.addEdge(
                vertexId0,
                vertexId1,
                ConflictReadGraphEdge());
            conflictReadGraph.addEdge(
                vertexId0ReverseComplement,
                vertexId1ReverseComplement,
                ConflictReadGraphEdge());
        }

    }
}



// This colors the ConflictReadGraph by walking the
// DirectedReadGraph.
void Assembler::colorConflictReadGraph()
{
    const bool debug = false;

    // Check that we have what we need.
    SHASTA_ASSERT(directedReadGraph.isOpen());
    SHASTA_ASSERT(conflictReadGraph.isOpen());

    // Types for vertices and edges of the two graphs we will use.
    using VertexId = DirectedReadGraph::VertexId;
    using EdgeId   = DirectedReadGraph::EdgeId;

    // We are assuming the two graphs use the same VertexId and EdgeId.
    static_assert(std::is_same<VertexId, ConflictReadGraph::VertexId>::value,
        "Unexpected VertexId discrepancy.");
    static_assert(std::is_same<VertexId, ConflictReadGraph::VertexId>::value,
        "Unexpected VertexId discrepancy.");

    // Initialize all vertex colors to invalid.
    const auto invalid = ConflictReadGraphVertex::invalid;
    const VertexId n = conflictReadGraph.vertices.size();
    for(VertexId vertexId=0; vertexId<n; vertexId++) {
        auto& vertex = conflictReadGraph.getVertex(vertexId);
        vertex.color = invalid;
    }


    // Create a table of vertices containing:
    // 0: VertexId
    // 1: Number of conflicts (degree in the conflict read graph).
    // 2: Degree in the read graph, counting only edges flagged as "keep".
    vector<tuple<VertexId, uint64_t, uint64_t> > vertexTable(n);
    for(VertexId v=0; v<n; v++) {
        vertexTable[v] = make_tuple(
            v,
            conflictReadGraph.degree(v),
            directedReadGraph.keptDegree(v));
    }

    // Sort it by increasing number of conflicts, then by decreasing degree.
    sort(
        vertexTable.begin(),
        vertexTable.end(),
        [](const auto& x, const auto& y)
        {
            return
                (get<1>(x) < get<1>(y))
                or
                ((get<1>(x) == get<1>(y)) and (get<2>(x) > get<2>(y)));
        }
        );



    // Data structures to keep track, at each iteration, of the vertices
    // we already encountered.
    vector<VertexId> encounteredVertices;
    vector<bool> wasEncountered(directedReadGraph.vertices.size(), false);

    // Data structures to keep track, at each iteration, of the vertices
    // that conflict with vertices we already encountered.
    vector<VertexId> forbiddenVertices;
    vector<bool> isForbidden(directedReadGraph.vertices.size(), false);

    // Other data structures used below.
    vector<VertexId> adjacentVertices;
    vector< pair<VertexId, uint64_t > > adjacentVerticesSortedByConflictCount;



    // Iterate over possible starting vertices in the order in which
    // they appear in the vertex table.
    uint64_t vertexTableIndex = 0;
    for(uint64_t iteration=0; ; iteration++) {
        if(vertexTableIndex == vertexTable.size()) {
            break;
        }

        // Find the next vertex that has not yet been colored.
        VertexId startVertexId = get<0>(vertexTable[vertexTableIndex++]);
        bool done = false;
        while(true) {
            if(not conflictReadGraph.getVertex(startVertexId).hasValidColor()) {
                break;
            }
            if(vertexTableIndex == n) {
                done = true;
                break;
            }
            startVertexId = get<0>(vertexTable[vertexTableIndex++]);
        }
        if(done) {
            break;
        }
        if(true) {
            cout << "Start iteration " << iteration <<
                " from " << ConflictReadGraph::getOrientedReadId(startVertexId);
            cout << " with number of conflicts " <<
                get<1>(vertexTable[vertexTableIndex-1]) <<
                " and kept degree  " <<
                get<2>(vertexTable[vertexTableIndex-1]) << endl;
        }


        // We use a process similar to a BFS starting at this vertex:
        // - When encountering a vertex that conflicts with another vertex
        //   we already encountered at this iteration, we skip it.
        // - When encountering a vertex that was already colored at a previous
        //   iteration (not just one colored at the current iteration), we skip it.
        // - When we enqueue neighbors of a vertex, we enqueue them in order
        //   of increasing number of conflicting vertices.


        // Initialize the BFS.
        std::queue<VertexId> q;
        q.push(startVertexId);
        wasEncountered[startVertexId] = true;
        encounteredVertices.push_back(startVertexId);
        conflictReadGraph.getVertex(startVertexId).color = iteration;


        // BFS loop.
        while(not q.empty()) {

            // Dequeue a vertex.
            const VertexId v0 = q.front();
            const OrientedReadId orientedReadId0 = ConflictReadGraph::getOrientedReadId(v0);
            if(debug) {
                cout << "Queue size " << q.size() << ", dequeued " << orientedReadId0 << endl;
            }
            q.pop();

            // If v0 is now forbidden, skip it.
            if(isForbidden[v0]) {
                // v0 was not forbidden when we enqueud it, but it is forbidden now.
                if(debug) {
                    cout << orientedReadId0 << " skipped because it is now forbidden." << endl;
                }
                continue;
            }

            // Give it a color equal to this iteration.
            conflictReadGraph.getVertex(v0).color = iteration;
            if(debug) {
                cout<< orientedReadId0 << " being colored " << iteration << endl;
            }

            // Gather adjacent vertices.
            directedReadGraph.findKeptAdjacent(v0, adjacentVertices);

            // Sort adjacent vertices by increasing number of conflicts.
            // Skip the ones that are conflicting with vertices we
            // already encountered at this iteration or that have
            // already been colored at a previous iteration.
            adjacentVerticesSortedByConflictCount.clear();
            for(const VertexId v1: adjacentVertices) {
                if(isForbidden[v1]) {
                    if(debug) {
                        cout << ConflictReadGraph::getOrientedReadId(v1) << " forbidden" << endl;
                    }
                    continue;
                }
                if(conflictReadGraph.vertices[v1].color != invalid) {
                    if(debug) {
                        cout << ConflictReadGraph::getOrientedReadId(v1) << " already colored" << endl;
                    }
                    continue;
                }
                if(wasEncountered[v1]) {
                    if(debug) {
                        cout << ConflictReadGraph::getOrientedReadId(v1) << " already encountered" << endl;
                    }
                    continue;
                }
                adjacentVerticesSortedByConflictCount.push_back(
                    make_pair(v1, conflictReadGraph.degree(v1)));
            }
            sort(
                adjacentVerticesSortedByConflictCount.begin(),
                adjacentVerticesSortedByConflictCount.end(),
                OrderPairsBySecondOnly<VertexId, uint64_t>());

            // Loop over adjacent vertices, in this order of increasing number of conflicts.
            for(const auto& p: adjacentVerticesSortedByConflictCount) {
                const VertexId v1 = p.first;

                // We know this vertex is not forbidden and was not already
                // colored at this or the previous iteration, so we can enqueue it now.
                SHASTA_ASSERT(not wasEncountered[v1]);
                wasEncountered[v1] = true;
                encounteredVertices.push_back(v1);
                q.push(v1);
                if(debug) {
                    cout << "Enqueued " << conflictReadGraph.getOrientedReadId(v1) << endl;
                }

                // Mark as forbidden the vertices that conflict with v1.
                for(const EdgeId e12: conflictReadGraph.edgesByVertex[v1]) {
                    const VertexId v2 = conflictReadGraph.otherVertex(e12, v1);
                    if(not isForbidden[v2]) {
                        isForbidden[v2] = true;
                        forbiddenVertices.push_back(v2);
                        if(debug) {
                            cout << "Marked as forbidden " <<
                                conflictReadGraph.getOrientedReadId(v2) << endl;
                        }
                    }
                }
            }
        }



        // Clean up data structures to prepare them for the next iteration.
        for(const VertexId v: forbiddenVertices) {
            SHASTA_ASSERT(isForbidden[v]);
            isForbidden[v] = false;
        }
        forbiddenVertices.clear();
        for(const VertexId v: encounteredVertices) {
            SHASTA_ASSERT(wasEncountered[v]);
            wasEncountered[v] = false;
        }
        encounteredVertices.clear();
    }



    // Check that all vertices were colored.
    for(VertexId v=0; v<n; v++) {
        SHASTA_ASSERT(conflictReadGraph.getVertex(v).hasValidColor());
    }

}



void Assembler::markDirectedReadGraphConflictEdges()
{
    // Check that we have what we need.
    SHASTA_ASSERT(directedReadGraph.isOpen());
    SHASTA_ASSERT(conflictReadGraph.isOpen());

    // Loop over all edges of the directed read graph.
    uint64_t invalidEdgeCount = 0;
    uint64_t invalidKeptEdgeCount = 0;
    uint64_t keptEdgeCount = 0;
    for(DirectedReadGraph::EdgeId edgeId=0; edgeId<directedReadGraph.edges.size(); edgeId++) {
        DirectedReadGraphEdge& edge = directedReadGraph.getEdge(edgeId);

        // Get the vertices of the DirectedReadGraph..
        const DirectedReadGraph::VertexId v0 = directedReadGraph.source(edgeId);
        const DirectedReadGraph::VertexId v1 = directedReadGraph.target(edgeId);

        // Get the corresponding OrientedReadId's.
        const OrientedReadId orientedReadId0 = OrientedReadId(OrientedReadId::Int(v0));
        const OrientedReadId orientedReadId1 = OrientedReadId(OrientedReadId::Int(v1));

        // Get the corresponding vertices of the ConclictReadGraph.
        const ConflictReadGraph::VertexId u0 = ConflictReadGraph::getVertexId(orientedReadId0);
        const ConflictReadGraph::VertexId u1 = ConflictReadGraph::getVertexId(orientedReadId1);
        const ConflictReadGraphVertex& cVertex0 = conflictReadGraph.getVertex(u0);
        const ConflictReadGraphVertex& cVertex1 = conflictReadGraph.getVertex(u1);
        SHASTA_ASSERT(cVertex0.hasValidColor());
        SHASTA_ASSERT(cVertex1.hasValidColor());

        // With current numbering, the vertex ids should be the same.
        SHASTA_ASSERT(u0 == v0);
        SHASTA_ASSERT(u1 == v1);

        // Figure out if this a conflict edge.
        edge.isConflict = (cVertex0.color != cVertex1.color);

        if(edge.isConflict) {
            ++invalidEdgeCount;
        }

        if(edge.keep) {
            ++keptEdgeCount;
            if(edge.isConflict) {
                ++invalidKeptEdgeCount;
            }
        }
    }

    cout << "Directed read graph edge counts:" << endl;
    cout << "    Total " << directedReadGraph.edges.size() << endl;
    cout << "    Kept for marker graph creation " << keptEdgeCount << endl;
    cout << "    Marked as conflict " << invalidEdgeCount << endl;
    cout << "    Kept for marker graph creation and marked as conflict " << invalidKeptEdgeCount << endl;
}
