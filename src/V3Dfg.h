// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Data flow graph (DFG) representation of logic
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2022 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// This is a data-flow graph based representation of combinational logic,
// the main difference from a V3Graph is that DfgVertex owns the storage
// of it's input edges (operands/sources/arguments), and can access each
// input edge directly by indexing, making modifications more efficient
// than the linked list based structures used by V3Graph.
//
// A bulk of the DfgVertex sub-types are generated by astgen, and are
// analogous to the correspondign AstNode sub-types.
//
// See also the internals documentation docs/internals.rst
//
//*************************************************************************

#ifndef VERILATOR_V3DFG_H_
#define VERILATOR_V3DFG_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Error.h"
#include "V3Hash.h"
#include "V3Hasher.h"
#include "V3List.h"

#include <functional>
#include <type_traits>
#include <unordered_map>
#include <vector>

class DfgVertex;
class DfgEdge;
class DfgVisitor;

//------------------------------------------------------------------------------

// Specialization of std::hash for a std::pair<const DfgVertex*, const DfgVertex*> for use below
template <>
struct std::hash<std::pair<const DfgVertex*, const DfgVertex*>> final {
    size_t operator()(const std::pair<const DfgVertex*, const DfgVertex*>& item) const {
        const size_t a = reinterpret_cast<std::uintptr_t>(item.first);
        const size_t b = reinterpret_cast<std::uintptr_t>(item.second);
        constexpr size_t halfWidth = 8 * sizeof(b) / 2;
        return a ^ ((b << halfWidth) | (b >> halfWidth));
    }
};

//------------------------------------------------------------------------------
// Dataflow graph
//------------------------------------------------------------------------------

class DfgGraph final {
    friend class DfgVertex;

    // MEMBERS
    size_t m_size = 0;  // Number of vertices in the graph
    V3List<DfgVertex*> m_vertices;  // The vertices in the graph
    // Parent of the graph (i.e.: the module containing the logic represented by this graph).
    AstModule* const m_modulep;
    const string m_name;  // Name of graph (for debugging)

public:
    // CONSTRUCTOR
    explicit DfgGraph(AstModule& module, const string& name = "");
    ~DfgGraph();
    VL_UNCOPYABLE(DfgGraph);

    // METHODS
private:
    // Add DfgVertex to this graph (assumes not yet contained).
    inline void addVertex(DfgVertex& vtx);
    // Remove DfgVertex form this graph (assumes it is contained).
    inline void removeVertex(DfgVertex& vtx);

public:
    // Number of vertices in this graph
    size_t size() const { return m_size; }

    // Parent module
    AstModule* modulep() const { return m_modulep; }

    // Name of this graph
    const string& name() const { return m_name; }

    // Calls given function 'f' for each vertex in the graph. It is safe to manipulate any vertices
    // in the graph, or to delete/unlink the vertex passed to 'f' during iteration. It is however
    // not safe to delete/unlink any vertex in the same graph other than the one passed to 'f'.
    inline void forEachVertex(std::function<void(DfgVertex&)> f);

    // 'const' variant of 'forEachVertex'. No mutation allowed.
    inline void forEachVertex(std::function<void(const DfgVertex&)> f) const;

    // Same as 'forEachVertex' but iterates in reverse order.
    inline void forEachVertexInReverse(std::function<void(DfgVertex&)> f);

    // Returns first vertex of type 'Vertex' that satisfies the given predicate 'p',
    // or nullptr if no such vertex exists in the graph.
    template <typename Vertex>
    inline Vertex* findVertex(std::function<bool(const Vertex&)> p) const;

    // Add contents of other graph to this graph. Leaves other graph empty.
    void addGraph(DfgGraph& other);

    // Topologically sort the list of vertices in this graph (such that 'forEachVertex' will
    // iterate in topological order), or reverse topologically if the passed boolean argument is
    // true. Returns true on success (the graph is acyclic and a topological order exists), false
    // if the graph is cyclic. If the graph is cyclic, the vertex ordering is not modified.
    bool sortTopologically(bool reverse = false);

    // Split this graph into individual components (unique sub-graphs with no edges between them).
    // Leaves 'this' graph empty.
    std::vector<std::unique_ptr<DfgGraph>> splitIntoComponents();

    // Apply the given function to all vertices in the graph. The function return value indicates
    // that a change has been made to the graph. Repeat until no changes reported.
    void runToFixedPoint(std::function<bool(DfgVertex&)> f);

    // Dump graph in Graphviz format into the given stream 'os'. 'label' is added to the name of
    // the graph which is included in the output.
    void dumpDot(std::ostream& os, const string& label = "") const;
    // Dump graph in Graphviz format into a new file with the given 'fileName'. 'label' is added to
    // the name of the graph which is included in the output.
    void dumpDotFile(const string& fileName, const string& label = "") const;
    // Dump graph in Graphviz format into a new automatically numbered debug file. 'label' is
    // added to the name of the graph, which is included in the file name and the output.
    void dumpDotFilePrefixed(const string& label = "") const;
    // Dump upstream (source) logic cone starting from given vertex into a file with the given
    // 'fileName'. 'name' is the name of the graph, which is included in the output.
    void dumpDotUpstreamCone(const string& fileName, const DfgVertex& vtx,
                             const string& name = "") const;
    // Dump all individual logic cones driving external variables in Graphviz format into separate
    // new automatically numbered debug files. 'label' is added to the name of the graph, which is
    // included in the file names and the output. This is useful for very large graphs that are
    // otherwise difficult to browse visually due to their size.
    void dumpDotAllVarConesPrefixed(const string& label = "") const;
};

//------------------------------------------------------------------------------
// Dataflow graph edge
//------------------------------------------------------------------------------

class DfgEdge final {
    friend class DfgVertex;
    template <size_t Arity>
    friend class DfgVertexWithArity;

    DfgEdge* m_nextp = nullptr;  // Next edge in sink list
    DfgEdge* m_prevp = nullptr;  // Previous edge in sink list
    DfgVertex* m_sourcep = nullptr;  // The source vertex driving this edge
    DfgVertex* const m_sinkp;  // The sink vertex. The sink owns the edge, so immutable

    explicit DfgEdge(DfgVertex* sinkp)  // The sink vertices own the edges, hence private
        : m_sinkp{sinkp} {}

public:
    // The source (driver) of this edge
    DfgVertex* sourcep() const { return m_sourcep; }
    // The sink (consumer) of this edge
    DfgVertex* sinkp() const { return m_sinkp; }
    // Remove driver of this edge
    void unlinkSource();
    // Relink this edge to be driven from the given new source vertex
    void relinkSource(DfgVertex* newSourcep);
};

//------------------------------------------------------------------------------
// Dataflow graph vertex
//------------------------------------------------------------------------------

// Reuse the generated type constants
using DfgType = VNType;

// Base data flow graph vertex
class DfgVertex VL_NOT_FINAL {
    friend class DfgGraph;
    friend class DfgEdge;
    friend class DfgVisitor;

    // STATE
    V3ListEnt<DfgVertex*> m_verticesEnt;  // V3List handle of this vertex, kept under the DfgGraph
protected:
    DfgEdge* m_sinksp = nullptr;  // List of sinks of this vertex
    FileLine* const m_filelinep;  // Source location
    AstNodeDType* m_dtypep = nullptr;  // Data type of the result of this vertex
    const DfgType m_type;

    // CONSTRUCTOR
    DfgVertex(DfgGraph& dfg, FileLine* flp, AstNodeDType* dtypep, DfgType type);

public:
    virtual ~DfgVertex() = default;

    // METHODS
private:
    // Visitor accept method
    virtual void accept(DfgVisitor& v) = 0;

    // Part of Vertex equality only dependent on this vertex
    virtual bool selfEquals(const DfgVertex& that) const;

    // Part of Vertex hash only dependent on this vertex
    virtual V3Hash selfHash() const;

public:
    // Returns true if an AstNode with the given 'dtype' can be represented as a DfgVertex
    static bool isSupportedDType(const AstNodeDType* dtypep) {
        // Conservatively only support bit-vector like basic types and packed arrays of the same
        dtypep = dtypep->skipRefp();
        if (const AstBasicDType* const typep = VN_CAST(dtypep, BasicDType)) {
            return typep->keyword().isIntNumeric();
        }
        if (const AstPackArrayDType* const typep = VN_CAST(dtypep, PackArrayDType)) {
            return isSupportedDType(typep->subDTypep());
        }
        return false;
    }

    // Return data type used to represent any packed value of the given 'width'. All packed types
    // of a given width use the same canonical data type, as the only interesting information is
    // the total width.
    static AstNodeDType* dtypeForWidth(uint32_t width) {
        return v3Global.rootp()->typeTablep()->findLogicDType(width, width, VSigning::UNSIGNED);
    }

    // Return data type used to represent the type of 'nodep' when converted to a DfgVertex
    static AstNodeDType* dtypeFor(const AstNode* nodep) {
        UDEBUGONLY(UASSERT_OBJ(isSupportedDType(nodep->dtypep()), nodep, "Unsupported dtype"););
        // Currently all supported types are packed, so this is simple
        return dtypeForWidth(nodep->width());
    }

    // Source location
    FileLine* fileline() const { return m_filelinep; }
    // The data type of the result of the nodes
    AstNodeDType* dtypep() const { return m_dtypep; }

    // Width of result
    uint32_t width() const {
        // Everything supported is packed now, so we can just do this:
        return dtypep()->width();
    }

    // Cache type for 'equals' below
    using EqualsCache = std::unordered_map<std::pair<const DfgVertex*, const DfgVertex*>, bool>;

    // Vertex equality (based on this vertex and all upstream vertices feeding into this vertex).
    // Returns true, if the vertices can be substituted for each other without changing the
    // semantics of the logic. The 'cache' argument is used to store results to avoid repeat
    // evaluations, but it requires that the upstream sources of the compared vertices do not
    // change between invocations.
    bool equals(const DfgVertex& that, EqualsCache& cache) const;

    // Uncached version of 'equals'
    bool equals(const DfgVertex& that) const {
        EqualsCache cache;  // Still cache recursive calls within this invocation
        return equals(that, cache);
    }

    // Cache type for 'hash' below
    using HashCache = std::unordered_map<const DfgVertex*, V3Hash>;

    // Hash of vertex (depends on this vertex and all upstream vertices feeding into this vertex).
    // The 'cache' argument is used to store results to avoid repeat evaluations, but it requires
    // that the upstream sources of the vertex do not change between invocations.
    V3Hash hash(HashCache& cache) const;

    // Uncached version of 'hash'
    V3Hash hash() const {
        HashCache cache;  // Still cache recursive calls within this invocation
        return hash(cache);
    }

    // Source edges of this vertex
    virtual std::pair<DfgEdge*, size_t> sourceEdges() { return {nullptr, 0}; }

    // Source edges of this vertex
    virtual std::pair<const DfgEdge*, size_t> sourceEdges() const { return {nullptr, 0}; }

    // Arity (number of sources) of this vertex
    size_t arity() const { return sourceEdges().second; }

    // Predicate: has 1 or more sinks
    bool hasSinks() const { return m_sinksp != nullptr; }

    // Predicate: has 2 or more sinks
    bool hasMultipleSinks() const { return m_sinksp && m_sinksp->m_nextp; }

    // Fanout (number of sinks) of this vertex (expensive to compute)
    uint32_t fanout() const;

    // Unlink from container (graph or builder), then delete this vertex
    void unlinkDelete(DfgGraph& dfg);

    // Relink all sinks to be driven from the given new source
    void replaceWith(DfgVertex* newSourcep);

    // Calls given function 'f' for each source vertex of this vertex
    // Unconnected source edges are not iterated.
    inline void forEachSource(std::function<void(const DfgVertex&)> f) const;

    // Calls given function 'f' for each source edge of this vertex. Also passes source index.
    inline void forEachSourceEdge(std::function<void(DfgEdge&, size_t)> f);

    // Calls given function 'f' for each source edge of this vertex. Also passes source index.
    inline void forEachSourceEdge(std::function<void(const DfgEdge&, size_t)> f) const;

    // Calls given function 'f' for each sink vertex of this vertex
    inline void forEachSink(std::function<void(DfgVertex&)> f);

    // Calls given function 'f' for each sink vertex of this vertex
    inline void forEachSink(std::function<void(const DfgVertex&)> f) const;

    // Calls given function 'f' for each sink edge of this vertex.
    // Unlinking/deleting the given sink during iteration is safe, but not other sinks of this
    // vertex.
    inline void forEachSinkEdge(std::function<void(DfgEdge&)> f);

    // Calls given function 'f' for each sink edge of this vertex.
    inline void forEachSinkEdge(std::function<void(const DfgEdge&)> f) const;

    // Returns first sink vertex of type 'Vertex' which satisfies the given predicate 'p',
    // or nullptr if no such sink vertex exists
    template <typename Vertex>
    inline Vertex* findSink(std::function<bool(const Vertex&)> p) const;

    // Returns first sink vertex of type 'Vertex', or nullptr if no such sink vertex exists.
    // This is a special case of 'findSink' above with the predicate always true.
    template <typename Vertex>
    inline Vertex* findSink() const;

    // Is this a DfgConst that is all zeroes
    inline bool isZero() const;

    // Is this a DfgConst that is all ones
    inline bool isOnes() const;

    // Methods that allow DfgVertex to participate in error reporting/messaging
    void v3errorEnd(std::ostringstream& str) const { m_filelinep->v3errorEnd(str); }
    void v3errorEndFatal(std::ostringstream& str) const VL_ATTR_NORETURN {
        m_filelinep->v3errorEndFatal(str);
    }
    string warnContextPrimary() const { return fileline()->warnContextPrimary(); }
    string warnContextSecondary() const { return fileline()->warnContextSecondary(); }
    string warnMore() const { return fileline()->warnMore(); }
    string warnOther() const { return fileline()->warnOther(); }

    // Subtype test
    template <typename T>
    bool is() const {
        static_assert(std::is_base_of<DfgVertex, T>::value, "'T' must be a subtype of DfgVertex");
        return m_type == T::dfgType();
    }

    // Ensure subtype, then cast to that type
    template <typename T>
    T* as() {
        UASSERT_OBJ(is<T>(), this,
                    "DfgVertex is not of expected type, but instead has type '" << typeName()
                                                                                << "'");
        return static_cast<T*>(this);
    }
    template <typename T>
    const T* as() const {
        UASSERT_OBJ(is<T>(), this,
                    "DfgVertex is not of expected type, but instead has type '" << typeName()
                                                                                << "'");
        return static_cast<const T*>(this);
    }

    // Cast to subtype, or null if different
    template <typename T>
    T* cast() {
        return is<T>() ? static_cast<T*>(this) : nullptr;
    }
    template <typename T>
    const T* cast() const {
        return is<T>() ? static_cast<const T*>(this) : nullptr;
    }

    // Human-readable vertex type as string for debugging
    const string typeName() const { return m_type.ascii(); }

    // Human-readable name for source operand with given index for debugging
    virtual const string srcName(size_t idx) const = 0;
};

// DfgVertices are, well ... DfgVertices
template <>
constexpr bool DfgVertex::is<DfgVertex>() const {
    return true;
}
template <>
constexpr DfgVertex* DfgVertex::as<DfgVertex>() {
    return this;
}
template <>
constexpr const DfgVertex* DfgVertex::as<DfgVertex>() const {
    return this;
}
template <>
constexpr DfgVertex* DfgVertex::cast<DfgVertex>() {
    return this;
}
template <>
constexpr const DfgVertex* DfgVertex::cast<DfgVertex>() const {
    return this;
}

template <size_t Arity>
class DfgVertexWithArity VL_NOT_FINAL : public DfgVertex {
    static_assert(1 <= Arity && Arity <= 4, "Arity must be between 1 and 4 inclusive");

    // Uninitialized storage for source edges
    typename std::aligned_storage<sizeof(DfgEdge[Arity]), alignof(DfgEdge[Arity])>::type
        m_sourceEdges;

    constexpr DfgEdge& sourceEdge(size_t index) {
        return reinterpret_cast<DfgEdge*>(&m_sourceEdges)[index];
    }
    constexpr const DfgEdge& sourceEdge(size_t index) const {
        return reinterpret_cast<const DfgEdge*>(&m_sourceEdges)[index];
    }

protected:
    DfgVertexWithArity<Arity>(DfgGraph& dfg, FileLine* flp, AstNodeDType* dtypep, DfgType type)
        : DfgVertex{dfg, flp, dtypep, type} {
        // Initialize source edges
        for (size_t i = 0; i < Arity; ++i) new (&sourceEdge(i)) DfgEdge{this};
    }

    virtual ~DfgVertexWithArity<Arity>() = default;

public:
    std::pair<DfgEdge*, size_t> sourceEdges() override {  //
        return {&sourceEdge(0), Arity};
    }
    std::pair<const DfgEdge*, size_t> sourceEdges() const override {
        return {&sourceEdge(0), Arity};
    }

    template <size_t Index>
    DfgVertex* source() const {
        static_assert(Index < Arity, "Source index out of range");
        return sourceEdge(Index).m_sourcep;
    }

    template <size_t Index>
    void relinkSource(DfgVertex* newSourcep) {
        static_assert(Index < Arity, "Source index out of range");
        UASSERT_OBJ(sourceEdge(Index).m_sinkp == this, this, "Inconsistent");
        sourceEdge(Index).relinkSource(newSourcep);
    }

    // Named source getter/setter for unary vertices
    template <size_t A = Arity>
    typename std::enable_if<A == 1, DfgVertex*>::type srcp() const {
        static_assert(A == Arity, "Should not be changed");
        return source<0>();
    }
    template <size_t A = Arity>
    typename std::enable_if<A == 1, void>::type srcp(DfgVertex* vtxp) {
        static_assert(A == Arity, "Should not be changed");
        relinkSource<0>(vtxp);
    }

    // Named source getter/setter for binary vertices
    template <size_t A = Arity>
    typename std::enable_if<A == 2, DfgVertex*>::type lhsp() const {
        static_assert(A == Arity, "Should not be changed");
        return source<0>();
    }
    template <size_t A = Arity>
    typename std::enable_if<A == 2, void>::type lhsp(DfgVertex* vtxp) {
        static_assert(A == Arity, "Should not be changed");
        relinkSource<0>(vtxp);
    }

    template <size_t A = Arity>
    typename std::enable_if<A == 2, DfgVertex*>::type rhsp() const {
        static_assert(A == Arity, "Should not be changed");
        return source<1>();
    }
    template <size_t A = Arity>
    typename std::enable_if<A == 2, void>::type rhsp(DfgVertex* vtxp) {
        static_assert(A == Arity, "Should not be changed");
        relinkSource<1>(vtxp);
    }
};

//------------------------------------------------------------------------------
// Vertex classes
//------------------------------------------------------------------------------

class DfgVar final : public DfgVertexWithArity<1> {
    friend class DfgVertex;
    friend class DfgVisitor;

    AstVar* const m_varp;  // The AstVar associated with this vertex (not owned by this vertex)
    FileLine* m_assignmentFlp;  // The FileLine of the original assignment driving this var
    bool m_hasModRefs = false;  // This AstVar is referenced outside the DFG, but in the module
    bool m_hasExtRefs = false;  // This AstVar is referenced from outside the module

    void accept(DfgVisitor& visitor) override;
    bool selfEquals(const DfgVertex& that) const override;
    V3Hash selfHash() const override;
    static constexpr DfgType dfgType() { return DfgType::atVar; };

public:
    DfgVar(DfgGraph& dfg, AstVar* varp)
        : DfgVertexWithArity<1>{dfg, varp->fileline(), dtypeFor(varp), dfgType()}
        , m_varp{varp} {}

    AstVar* varp() const { return m_varp; }
    FileLine* assignmentFileline() const { return m_assignmentFlp; }
    void assignmentFileline(FileLine* flp) { m_assignmentFlp = flp; }
    bool hasModRefs() const { return m_hasModRefs; }
    void setHasModRefs() { m_hasModRefs = true; }
    bool hasExtRefs() const { return m_hasExtRefs; }
    void setHasExtRefs() { m_hasExtRefs = true; }
    bool hasRefs() const { return m_hasModRefs || m_hasExtRefs; }

    DfgVertex* driverp() const { return srcp(); }
    void driverp(DfgVertex* vtxp) { srcp(vtxp); }

    // Variable cannot be removed, even if redundant in the DfgGraph (might be used externally)
    bool keep() const {
        // Keep if referenced outside this module
        if (hasExtRefs()) return true;
        // Keep if traced
        if (v3Global.opt.trace() && varp()->isTrace()) return true;
        // Keep if public
        if (varp()->isSigPublic()) return true;
        // Otherwise it can be removed
        return false;
    }

    const string srcName(size_t) const override { return "driverp"; }
};

class DfgConst final : public DfgVertex {
    friend class DfgVertex;
    friend class DfgVisitor;

    AstConst* const m_constp;  // The AstConst associated with this vertex (owned by this vertex)

    void accept(DfgVisitor& visitor) override;
    bool selfEquals(const DfgVertex& that) const override;
    V3Hash selfHash() const override;
    static constexpr DfgType dfgType() { return DfgType::atConst; };

public:
    DfgConst(DfgGraph& dfg, AstConst* constp)
        : DfgVertex{dfg, constp->fileline(), dtypeFor(constp), dfgType()}
        , m_constp{constp} {}

    ~DfgConst() { VL_DO_DANGLING(m_constp->deleteTree(), m_constp); }

    AstConst* constp() const { return m_constp; }
    V3Number& num() const { return m_constp->num(); }

    uint32_t toU32() const { return num().toUInt(); }
    int32_t toI32() const { return num().toSInt(); }

    bool isZero() const { return num().isEqZero(); }
    bool isOnes() const { return num().isEqAllOnes(width()); }

    const string srcName(size_t) const override {  // LCOV_EXCL_START
        VL_UNREACHABLE;
        return "";
    }  // LCOV_EXCL_STOP
};

// The rest of the DfgVertex subclasses are generated by 'astgen' from AstNodeMath nodes
#include "V3Dfg__gen_vertex_classes.h"

//------------------------------------------------------------------------------
// Dfg vertex visitor
//------------------------------------------------------------------------------

class DfgVisitor VL_NOT_FINAL {
public:
    // Dispatch to most specific 'visit' method on 'vtxp'
    void iterate(DfgVertex* vtxp) { vtxp->accept(*this); }

    virtual void visit(DfgVar* vtxp);
    virtual void visit(DfgConst* vtxp);
#include "V3Dfg__gen_visitor_decls.h"
};

//------------------------------------------------------------------------------
// Inline method definitions
//------------------------------------------------------------------------------

void DfgGraph::addVertex(DfgVertex& vtx) {
    ++m_size;
    vtx.m_verticesEnt.pushBack(m_vertices, &vtx);
}

void DfgGraph::removeVertex(DfgVertex& vtx) {
    --m_size;
    vtx.m_verticesEnt.unlink(m_vertices, &vtx);
}

void DfgGraph::forEachVertex(std::function<void(DfgVertex&)> f) {
    for (DfgVertex *vtxp = m_vertices.begin(), *nextp; vtxp; vtxp = nextp) {
        nextp = vtxp->m_verticesEnt.nextp();
        f(*vtxp);
    }
}

void DfgGraph::forEachVertex(std::function<void(const DfgVertex&)> f) const {
    for (const DfgVertex* vtxp = m_vertices.begin(); vtxp; vtxp = vtxp->m_verticesEnt.nextp()) {
        f(*vtxp);
    }
}

void DfgGraph::forEachVertexInReverse(std::function<void(DfgVertex&)> f) {
    for (DfgVertex *vtxp = m_vertices.rbegin(), *nextp; vtxp; vtxp = nextp) {
        nextp = vtxp->m_verticesEnt.prevp();
        f(*vtxp);
    }
}

template <typename Vertex>
Vertex* DfgGraph::findVertex(std::function<bool(const Vertex&)> p) const {
    static_assert(std::is_base_of<DfgVertex, Vertex>::value,
                  "'Vertex' must be subclass of 'DfgVertex'");
    for (DfgVertex* vtxp = m_vertices.begin(); vtxp; vtxp = vtxp->m_verticesEnt.nextp()) {
        if (Vertex* const vvtxp = vtxp->cast<Vertex>()) {
            if (p(*vvtxp)) return vvtxp;
        }
    }
    return nullptr;
}

void DfgVertex::forEachSource(std::function<void(const DfgVertex&)> f) const {
    const auto pair = sourceEdges();
    const DfgEdge* const edgesp = pair.first;
    const size_t arity = pair.second;
    for (size_t i = 0; i < arity; ++i) {
        if (DfgVertex* const sourcep = edgesp[i].m_sourcep) f(*sourcep);
    }
}

void DfgVertex::forEachSink(std::function<void(DfgVertex&)> f) {
    for (const DfgEdge* edgep = m_sinksp; edgep; edgep = edgep->m_nextp) f(*edgep->m_sinkp);
}

void DfgVertex::forEachSink(std::function<void(const DfgVertex&)> f) const {
    for (const DfgEdge* edgep = m_sinksp; edgep; edgep = edgep->m_nextp) f(*edgep->m_sinkp);
}

void DfgVertex::forEachSourceEdge(std::function<void(DfgEdge&, size_t)> f) {
    const auto pair = sourceEdges();
    DfgEdge* const edgesp = pair.first;
    const size_t arity = pair.second;
    for (size_t i = 0; i < arity; ++i) f(edgesp[i], i);
}

void DfgVertex::forEachSourceEdge(std::function<void(const DfgEdge&, size_t)> f) const {
    const auto pair = sourceEdges();
    const DfgEdge* const edgesp = pair.first;
    const size_t arity = pair.second;
    for (size_t i = 0; i < arity; ++i) f(edgesp[i], i);
}

void DfgVertex::forEachSinkEdge(std::function<void(DfgEdge&)> f) {
    for (DfgEdge *edgep = m_sinksp, *nextp; edgep; edgep = nextp) {
        nextp = edgep->m_nextp;
        f(*edgep);
    }
}

void DfgVertex::forEachSinkEdge(std::function<void(const DfgEdge&)> f) const {
    for (DfgEdge *edgep = m_sinksp, *nextp; edgep; edgep = nextp) {
        nextp = edgep->m_nextp;
        f(*edgep);
    }
}

template <typename Vertex>
Vertex* DfgVertex::findSink(std::function<bool(const Vertex&)> p) const {
    static_assert(std::is_base_of<DfgVertex, Vertex>::value,
                  "'Vertex' must be subclass of 'DfgVertex'");
    for (DfgEdge* edgep = m_sinksp; edgep; edgep = edgep->m_nextp) {
        if (Vertex* const sinkp = edgep->m_sinkp->cast<Vertex>()) {
            if (p(*sinkp)) return sinkp;
        }
    }
    return nullptr;
}

template <typename Vertex>
Vertex* DfgVertex::findSink() const {
    static_assert(!std::is_same<DfgVertex, Vertex>::value,
                  "'Vertex' must be proper subclass of 'DfgVertex'");
    return findSink<Vertex>([](const Vertex&) { return true; });
}

bool DfgVertex::isZero() const {
    if (const DfgConst* const constp = cast<DfgConst>()) return constp->isZero();
    return false;
}

bool DfgVertex::isOnes() const {
    if (const DfgConst* const constp = cast<DfgConst>()) return constp->isOnes();
    return false;
}

#endif
