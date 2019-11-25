// Shasta.
#include "LocalDirectedReadGraph.hpp"
using namespace shasta;

// Boost libraries.
#include <boost/graph/graphviz.hpp>

// Standard library.
#include "fstream.hpp"
#include "stdexcept.hpp"
#include "utility.hpp"



void LocalDirectedReadGraph::addVertex(
    OrientedReadId orientedReadId,
    uint32_t baseCount,
    uint32_t markerCount,
    uint32_t distance,
    bool isContained)
{
    // Check that we don't already have a vertex with this OrientedReadId.
    SHASTA_ASSERT(vertexMap.find(orientedReadId) == vertexMap.end());

    // Create the vertex.
    const vertex_descriptor v = add_vertex(LocalDirectedReadGraphVertex(
        orientedReadId, baseCount, markerCount, distance, isContained), *this);

    // Store it in the vertex map.
    vertexMap.insert(make_pair(orientedReadId, v));
}



void LocalDirectedReadGraph::addEdge(
    OrientedReadId orientedReadId0,
    OrientedReadId orientedReadId1,
    const AlignmentInfo& alignmentInfo,
    bool involvesTwoContainedVertices,
    bool involvesOneContainedVertex,
    bool wasRemovedByTransitiveReduction,
    uint32_t commonNeighborCount)
{
    // Find the vertices corresponding to these two OrientedReadId.
    const auto it0 = vertexMap.find(orientedReadId0);
    SHASTA_ASSERT(it0 != vertexMap.end());
    const vertex_descriptor v0 = it0->second;
    const auto it1 = vertexMap.find(orientedReadId1);
    SHASTA_ASSERT(it1 != vertexMap.end());
    const vertex_descriptor v1 = it1->second;

    // Add the edge.
    add_edge(v0, v1,
        LocalDirectedReadGraphEdge(alignmentInfo,
            involvesTwoContainedVertices,
            involvesOneContainedVertex,
            wasRemovedByTransitiveReduction,
            commonNeighborCount),
        *this);
}



uint32_t LocalDirectedReadGraph::getDistance(OrientedReadId orientedReadId) const
{
    const auto it = vertexMap.find(orientedReadId);
    SHASTA_ASSERT(it != vertexMap.end());
    const vertex_descriptor v = it->second;
    return (*this)[v].distance;
}



bool LocalDirectedReadGraph::vertexExists(OrientedReadId orientedReadId) const
{
   return vertexMap.find(orientedReadId) != vertexMap.end();
}



// Write the graph in Graphviz format.
void LocalDirectedReadGraph::write(
    const string& fileName,
    uint32_t maxDistance,
    double vertexScalingFactor,
    double edgeThicknessScalingFactor,
    double edgeArrowScalingFactor
    ) const
{
    ofstream outputFileStream(fileName);
    if(!outputFileStream) {
        throw runtime_error("Error opening " + fileName);
    }
    write(outputFileStream, maxDistance, vertexScalingFactor,
        edgeThicknessScalingFactor, edgeArrowScalingFactor);
}
void LocalDirectedReadGraph::write(
    ostream& s,
    uint32_t maxDistance,
    double vertexScalingFactor,
    double edgeThicknessScalingFactor,
    double edgeArrowScalingFactor) const
{
    Writer writer(*this, maxDistance, vertexScalingFactor,
        edgeThicknessScalingFactor, edgeArrowScalingFactor);
    boost::write_graphviz(s, *this, writer, writer, writer,
        boost::get(&LocalDirectedReadGraphVertex::orientedReadIdValue, *this));
}

LocalDirectedReadGraph::Writer::Writer(
    const LocalDirectedReadGraph& graph,
    uint32_t maxDistance,
    double vertexScalingFactor,
    double edgeThicknessScalingFactor,
    double edgeArrowScalingFactor) :
    graph(graph),
    maxDistance(maxDistance),
    vertexScalingFactor(vertexScalingFactor),
    edgeThicknessScalingFactor(edgeThicknessScalingFactor),
    edgeArrowScalingFactor(edgeArrowScalingFactor)
{
}



void LocalDirectedReadGraph::Writer::operator()(std::ostream& s) const
{
    s << "layout=sfdp;\n";
    s << "ratio=expand;\n";
    s << "node [shape=point];\n";
    s << "edge [penwidth=\"0.2\"];\n";

    // This turns off the tooltip on the graph.
    s << "tooltip = \" \";\n";
}


void LocalDirectedReadGraph::Writer::operator()(std::ostream& s, vertex_descriptor v) const
{
    const LocalDirectedReadGraphVertex& vertex = graph[v];
    const OrientedReadId orientedReadId(vertex.orientedReadId);

    // Tooltip.
    s <<
        "["
        " tooltip=\"Read " << orientedReadId << ", " <<
        vertex.baseCount << " bases, " << vertex.markerCount <<
        " markers, distance " << vertex.distance << vertex.additionalToolTipText << "\"" <<
        " URL=\"exploreRead?readId=" << orientedReadId.getReadId() <<
        "&strand=" << orientedReadId.getStrand() <<
        "\"" <<
        " width=" << vertexScalingFactor * sqrt(1.e-6 * double(vertex.markerCount)) <<
        " height=" << vertexScalingFactor * sqrt(1.e-6 * double(vertex.markerCount));

    // Color.
    if(vertex.distance == 0) {
        s << " color=green";
    } else if(vertex.distance == maxDistance) {
        s << " color=cyan";
    } else if(vertex.isContained) {
        s << " color=red";
    } else {
        s << " color=black";
    }

    // Shape.
    if(not vertex.additionalToolTipText.empty()) {
        s << " shape=diamond style=filled label=\"\"";
    }

    s << "]";
}



void LocalDirectedReadGraph::Writer::operator()(std::ostream& s, edge_descriptor e) const
{
    const LocalDirectedReadGraphEdge& edge = graph[e];
    const vertex_descriptor v0 = source(e, graph);
    const vertex_descriptor v1 = target(e, graph);
    const LocalDirectedReadGraphVertex& vertex0 = graph[v0];
    const LocalDirectedReadGraphVertex& vertex1 = graph[v1];

    s << "[";

    s <<
        "tooltip=\"" << vertex0.orientedReadId << "->" <<
        vertex1.orientedReadId <<
        ", " << edge.alignmentInfo.markerCount << " aligned markers, centers offset " <<
        std::setprecision(6) << edge.alignmentInfo.offsetAtCenter() <<
        " aligned fraction " <<
        std::setprecision(3) <<
        edge.alignmentInfo.alignedFraction(0) << " " <<
        edge.alignmentInfo.alignedFraction(1) <<
        ", common neighbors " << edge.commonNeighborCount <<
        "\"";

    s << " penwidth=\"" << edgeThicknessScalingFactor << "\"";
    s << " arrowsize=\"" << edgeArrowScalingFactor << "\"";

    if(edge.involvesTwoContainedVertices) {
        s << " color=\"#ff00007f\""; // Partially transparent red.
    } else if(edge.involvesOneContainedVertex) {
        s << " color=\"#00ffff7f\""; // Partially transparent cyan.
    } else if(edge.wasRemovedByTransitiveReduction) {
        s << " color=\"#00ff007f\""; // Partially transparent green.
    } else {
        s << " color=black";
    }

    s << "]";
}
