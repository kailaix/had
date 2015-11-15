/**
    HAD is a single header C++ reverse-mode automatic differentiation library using operator overloading, with focus on 
    second-order derivatives (Hessian).  
    It implements the edge_pushing algorithm (see "Hessian Matrices via Automatic Differentiation", 
    Gower and Mello 2010) to efficiently compute the second derivatives.
    The computation graph is built implicitly similar to the Adept library (http://www.met.reading.ac.uk/clouds/adept/),
    only the floating point weights are stored.
    HAD also stores the derivatives coefficients in a STL vector when recording the function, reducing the number of
    memory allocation calls.
    Currently HAD does not support "checkpointing" which is important for very very long functions.

    See test.cpp for usage.
    The library depends on Eigen (http://eigen.tuxfamily.org/).

    Below is a quick comparison to other auto-diff libraries:

    Adept: http://www.met.reading.ac.uk/clouds/adept/
    Adept uses expression templates and implicit stored computation graph to better utilize compiler optimization.
    For first derivatives computation, it can be faster than HAD because HAD does not use expression templates.
    However, Adept does not support second derivatives.

    CppAD: http://www.coin-or.org/CppAD/ & ADOL-C: https://projects.coin-or.org/ADOL-C
    CppAD and ADOL-C store the whole computation graph with symbolic representation to compute the derivatives.
    This makes them able to record the computation graph once, and feed it with different input variables to obtain 
    derivatives at different points.  
    However, since the derivatives comptuation now requires a table lookup with the type of the symbol, they are less 
    efficient compare to HAD.
    They also need to re-record the computation graph once the conditions for the if-else statements or loops are changed.
    Most crucially, they compute the Hessian by interleaving forward and reverse automatic differentiation, 
    which does not fully utilize the symmetry of Hessian computation graph.
    
    Stan-math: https://github.com/stan-dev/math
    Stan-math is a carefully tuned auto-diff library with comparable performance with Adept and supports second and even 
    higher-order derivatives computation.
    It also has a full compatability with the Eigen library (for HAD, I haven't intensively tested it yet).
    However, for second-order derivatives they also resort to interleaving between forward and reverse passes.  
    In order to obtain a n-dimensional Hessian matrix, they have to run n forward-reverse passes of the same function, 
    which results in many repeated computation.  They also do not utilize the symmetry of Hessian.

    autodiff.h: http://www.mitsuba-renderer.org/files/eigen/autodiff.h
    autodiff is a single header automatic differentiation library for C++ which can do first and second-order derivatives 
    just like HAD.
    However, autodiff uses forward-mode automatic differentiation which is known to be inefficient compare to reverse-mode 
    when the output dimension is smaller than the input dimension (O(n^2) v.s. O(n) where n is the input dimension of the
    Hessian matrix).
    The fact that HAD stores the derivatives coefficients in a STL vector also make it more efficient.

    Author: Tzu-Mao Li


    The MIT License (MIT)

    Copyright (c) 2015 Tzu-Mao Li

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
**/

#ifndef HAD_H__
#define HAD_H__

#include <vector>
#include <cmath>
#include <Eigen/Sparse>

namespace had {

// Change the following line if you want to use single precision floats
typedef double Real; 
typedef unsigned int VertexId;

struct ADGraph;
struct AReal;

extern __thread ADGraph* g_ADGraph;
// Declare this in your .cpp source
#define DECLARE_ADGRAPH() namespace had { __thread ADGraph* g_ADGraph = 0; }

AReal NewAReal(const Real val);

struct AReal {
    AReal(const Real val) {
        *this = NewAReal(val);
    }

    AReal(const Real val, const VertexId varId) : 
        val(val), varId(varId) {}

    Real val;
    VertexId varId;
};

struct ADEdge {
    ADEdge() {}
    ADEdge(const VertexId to, const Real w = Real(0.0)) : 
        to(to), w(w) {}

    VertexId to;
    Real w;
};

// We assume there is at most 2 outgoing edges from this vertex
struct ADVertex {
    ADVertex(const VertexId newId) {
        e1 = e2 = ADEdge(newId);
        w = soW = Real(0.0);
    }

    // If ei.to == the id of this vertex, then the edge does not exist
    ADEdge e1, e2;
    // first-order weight
    Real w;
    // second-order weights
    // for vertex with single outgoing edge, 
    // soW represents the second-order weight of the conntecting vertex (d^2f/dx^2)
    // for vertex with two outgoing edges,
    // soW represents the second-order weight between the conntecting vertices (d^2f/dxdy)
    // the system assumes d^2f/dx^2 & d^2f/dy^2 are both zero in the two outgoing edges case to save memory
    Real soW;
};

struct ADGraph {
    ADGraph() {
        g_ADGraph = this;
    }

    void Clear() {
        vertices.clear();
        soEdges.setZero();
    }

    std::vector<ADVertex> vertices;
    Eigen::SparseMatrix<Real> soEdges;
};

inline AReal NewAReal(const Real val) {
    std::vector<ADVertex> &vertices = g_ADGraph->vertices;
    VertexId newId = vertices.size();
    vertices.push_back(ADVertex(newId));
    return AReal(val, newId);
}

inline void AddEdge(const AReal &c, const AReal &p, 
                    const Real w, const Real soW) {
    ADVertex &v = g_ADGraph->vertices[c.varId];
    v.e1 = ADEdge(p.varId, w);
    v.soW = soW;
}
inline void AddEdge(const AReal &c, 
                    const AReal &p1, const AReal &p2, 
                    const Real w1, const Real w2,
                    const Real soW) {
    ADVertex &v = g_ADGraph->vertices[c.varId];
    v.e1 = ADEdge(p1.varId, w1);
    v.e2 = ADEdge(p2.varId, w2);
    v.soW = soW;
}

// Second-order edges (stores at the one with lower index so that we will always visit it)
inline void AddSoEdge(const VertexId i, const VertexId j, const Real w) {
    g_ADGraph->soEdges.coeffRef(std::min(i, j), std::max(i, j)) += w;
}

////////////////////// Addition ///////////////////////////
inline AReal operator+(const AReal &l, const AReal &r) {
    AReal ret = NewAReal(l.val + r.val);
    AddEdge(ret, l, r, Real(1.0), Real(1.0), Real(0.0));
    return ret;
}
inline AReal operator+(const AReal &l, const Real r) {
    AReal ret = NewAReal(l.val + r);
    AddEdge(ret, l, Real(1.0), Real(0.0));
    return ret;
}
inline AReal operator+(const Real l, const AReal &r) {
    return r + l;
}
inline AReal& operator+=(AReal &l, const AReal &r) {
    return (l = l + r);
}
inline AReal& operator+=(AReal &l, const Real r) {
    return (l = l + r);
}
///////////////////////////////////////////////////////////

//////////////////// Subtraction //////////////////////////
inline AReal operator-(const AReal &l, const AReal &r) {
    AReal ret = NewAReal(l.val - r.val);
    AddEdge(ret, l, r, Real(1.0), -Real(1.0), Real(0.0));
    return ret;
}
inline AReal operator-(const AReal &l, const Real r) {
    AReal ret = NewAReal(l.val - r);
    AddEdge(ret, l, Real(1.0), Real(0.0));
    return ret;
}
inline AReal operator-(const Real l, const AReal &r) {
    AReal ret = NewAReal(l - r.val);
    AddEdge(ret, r, Real(-1.0), Real(0.0));
    return ret;
}
inline AReal& operator-=(AReal &l, const AReal &r) {
    return (l = l - r);
}
inline AReal& operator-=(AReal &l, const Real r) {
    return (l = l - r);
}
///////////////////////////////////////////////////////////

////////////////// Multiplication /////////////////////////
inline AReal operator*(const AReal &l, const AReal &r) {
    AReal ret = NewAReal(l.val * r.val);
    AddEdge(ret, l, r, r.val, l.val, Real(1.0));
    return ret;
}
inline AReal operator*(const AReal &l, const Real r) {
    AReal ret = NewAReal(l.val * r);
    AddEdge(ret, l, r, Real(0.0));
    return ret;
}
inline AReal operator*(const Real l, const AReal &r) {
    return r * l;
}
inline AReal& operator*=(AReal &l, const AReal &r) {
    return (l = l * r);
}
inline AReal& operator*=(AReal &l, const Real r) {
    return (l = l * r);
}
///////////////////////////////////////////////////////////

////////////////// Inversion //////////////////////////////
inline AReal Inv(const AReal &x) {
    Real invX = Real(1.0) / x.val;
    Real invXSq = invX * invX;
    Real invXCu = invXSq * invX;
    AReal ret = NewAReal(invX);
    AddEdge(ret, x, -invXSq, Real(2.0) * invXCu);
    return ret;
}
inline Real Inv(const Real x) {
    return Real(1.0) / x;
}
///////////////////////////////////////////////////////////

////////////////// Division ///////////////////////////////
inline AReal operator/(const AReal &l, const AReal &r) {
    return l * Inv(r);
}
inline AReal operator/(const AReal &l, const Real r) {
    return l * Inv(r);
}
inline AReal operator/(const Real l, const AReal &r) {
    return l * Inv(r);
}
inline AReal& operator/=(AReal &l, const AReal &r) {
    return (l = l / r);
}
inline AReal& operator/=(AReal &l, const Real r) {
    return (l = l / r);
}
///////////////////////////////////////////////////////////

////////////////// Comparisons ////////////////////////////
inline bool operator<(const AReal &l, const AReal &r) {
    return l.val < r.val;
}
inline bool operator<=(const AReal &l, const AReal &r) {
    return l.val <= r.val;
}
inline bool operator>(const AReal &l, const AReal &r) {
    return l.val > r.val;
}
inline bool operator>=(const AReal &l, const AReal &r) {
    return l.val >= r.val;
}
inline bool operator==(const AReal &l, const AReal &r) {
    return l.val == r.val;
}
///////////////////////////////////////////////////////////

//////////////// Misc functions ///////////////////////////
inline AReal sqrt(const AReal &x) {
    Real sqrtX = std::sqrt(x.val);
    Real invSqrtX = Real(1.0) / sqrtX;
    AReal ret = NewAReal(sqrtX);
    AddEdge(ret, x, Real(0.5) * invSqrtX, - Real(0.25) * invSqrtX / x.val);
    return ret;
}
inline AReal pow(const AReal &x, const Real a) {
    Real powX = std::pow(x.val, a);
    AReal ret = NewAReal(powX);
    AddEdge(ret, x, a * std::pow(x.val, a - Real(1.0)),
                    a * (a - Real(1.0)) * std::pow(x.val, a - Real(2.0)));
    return ret;
}
inline AReal exp(const AReal &x) {
    Real expX = std::exp(x.val);
    AReal ret = NewAReal(expX);
    AddEdge(ret, x, expX, expX);
    return ret;
}
inline AReal log(const AReal &x) {
    Real logX = std::log(x.val);
    AReal ret = NewAReal(logX);
    Real invX = Real(1.0) / x.val;
    AddEdge(ret, x, invX, - invX * invX);
    return ret;
}
inline AReal sin(const AReal &x) {
    Real sinX = std::sin(x.val);
    AReal ret = NewAReal(sinX);
    AddEdge(ret, x, std::cos(x.val), -sinX);
    return ret;
}
inline AReal cos(const AReal &x) {
    Real cosX = std::cos(x.val);
    AReal ret = NewAReal(cosX);
    AddEdge(ret, x, -std::sin(x.val), -cosX);
    return ret;
}
inline AReal tan(const AReal &x) {
    Real tanX = std::tan(x.val);
    Real secX = Real(1.0) / std::cos(x.val);
    Real sec2X = secX * secX;
    AReal ret = NewAReal(tanX);
    AddEdge(ret, x, sec2X, Real(2.0) * tanX * sec2X);
    return ret;
}
inline AReal asin(const AReal &x) {
    Real asinX = std::asin(x.val);
    AReal ret = NewAReal(asinX);
    Real tmp = Real(1.0) / (Real(1.0) - x.val * x.val);
    Real sqrtTmp = std::sqrt(tmp);
    AddEdge(ret, x, sqrtTmp, x.val * sqrtTmp * tmp);
    return ret;
}
inline AReal acos(const AReal &x) {
    Real acosX = std::acos(x.val);
    AReal ret = NewAReal(acosX);
    Real tmp = Real(1.0) / (Real(1.0) - x.val * x.val);
    Real negSqrtTmp = -std::sqrt(tmp);
    AddEdge(ret, x, negSqrtTmp, x.val * negSqrtTmp * tmp);
    return ret;
}
///////////////////////////////////////////////////////////

inline void SetAdjoint(const AReal &v, const Real adj) {
    g_ADGraph->vertices[v.varId].w = adj;
}

inline Real GetAdjoint(const AReal &v) {
    return g_ADGraph->vertices[v.varId].w;
}

inline Real GetAdjoint(const AReal &i, const AReal &j) {
    return g_ADGraph->soEdges.coeffRef(std::min(i.varId, j.varId), std::max(i.varId, j.varId));
}

inline void PushEdge(const ADEdge &foEdge, const ADEdge &soEdge) {
    if (foEdge.to == soEdge.to) {
        AddSoEdge(foEdge.to, foEdge.to, Real(2.0) * foEdge.w * soEdge.w);
    } else {
        AddSoEdge(foEdge.to, soEdge.to, foEdge.w * soEdge.w);
    }
}

inline void PropagateAdjoint() {
    g_ADGraph->soEdges.resize(g_ADGraph->vertices.size(), g_ADGraph->vertices.size());
    // Any chance for SSE/AVX parallism?
    for (VertexId vid = g_ADGraph->vertices.size() - 1; vid > 0; vid--) {
        // Pushing
        ADVertex &vertex = g_ADGraph->vertices[vid];
        ADEdge &e1 = vertex.e1;
        ADEdge &e2 = vertex.e2;
        if (e1.to == vid) {
            continue;
        }
        for (Eigen::SparseMatrix<Real>::InnerIterator it(g_ADGraph->soEdges, vid);it;++it) {
            ADEdge soEdge(it.index(), it.value());
            if (soEdge.to != vid) {
                PushEdge(e1, soEdge);
                if (e2.to != vid) {
                    PushEdge(e2, soEdge);
                }
            } else { //soEdge.to == vid
                AddSoEdge(e1.to, e1.to, e1.w * e1.w * soEdge.w);
                if (e2.to != vid) {
                    AddSoEdge(e2.to, e2.to, e2.w * e2.w * soEdge.w);
                    // e1 must exists if e2 is not empty
                    AddSoEdge(e1.to, e2.to, 
                        (e1.to == e2.to) ? 2.0 * e1.w * e2.w * soEdge.w : 
                                                 e1.w * e2.w * soEdge.w);
                }
            }
        }
        // delete edges here?

        Real a = vertex.w;
        if (a != Real(0.0)) {
            // Creating
            if (vertex.soW != Real(0.0)) {
                if (e2.to == vid) { // single-edge
                    AddSoEdge(e1.to, e1.to, a * vertex.soW);
                } else {
                    AddSoEdge(e1.to, e2.to, 
                        (e1.to == e2.to) ? 2.0 * a * vertex.soW : a * vertex.soW);
                }
            }

            // Adjoint
            vertex.w = Real(0.0);
            g_ADGraph->vertices[e1.to].w += a * e1.w;
            if (e2.to != vid) {
                g_ADGraph->vertices[e2.to].w += a * e2.w;
            }
        }
    }
}

} //namespace had

#endif // HAD_H__