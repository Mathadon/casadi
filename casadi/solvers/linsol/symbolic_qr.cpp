/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "symbolic_qr.hpp"

#ifdef WITH_DL
#include <cstdlib>
#endif // WITH_DL

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_LINSOL_SYMBOLICQR_EXPORT
  casadi_register_linsol_symbolicqr(LinsolInternal::Plugin* plugin) {
    plugin->creator = SymbolicQr::creator;
    plugin->name = "symbolicqr";
    plugin->doc = SymbolicQr::meta_doc.c_str();
    plugin->version = 31;
    return 0;
  }

  extern "C"
  void CASADI_LINSOL_SYMBOLICQR_EXPORT casadi_load_linsol_symbolicqr() {
    LinsolInternal::registerPlugin(casadi_register_linsol_symbolicqr);
  }

  SymbolicQr::SymbolicQr(const std::string& name) :
    LinsolInternal(name) {
  }

  SymbolicQr::~SymbolicQr() {
    clear_memory();
  }

  Options SymbolicQr::options_
  = {{&FunctionInternal::options_},
    {{"codegen",
      {OT_BOOL,
       "C-code generation"}},
      {"compiler",
      {OT_STRING,
       "Compiler command to be used for compiling generated code"}}
   }
  };

  void SymbolicQr::init(const Dict& opts) {
    // Call the base class initializer
    LinsolInternal::init(opts);

    // Default options
    bool codegen = false;

    // Read options
    for (auto&& op : opts) {
      if (op.first=="codegen") {
        codegen = op.second;
      } else if (op.first=="compiler") {
        casadi_error("Option \"compiler\" has been removed");
      }
    }

    // Codegen options
    if (codegen) {
      fopts_["compiler"] = compilerplugin_;
      fopts_["jit_options"] = jit_options_;
    }
  }

  void SymbolicQr::init_memory(void* mem) const {
    LinsolInternal::init_memory(mem);
  }

  void SymbolicQr::reset(void* mem, const int* sp) const {
    LinsolInternal::reset(mem, sp);
    auto m = static_cast<SymbolicQrMemory*>(mem);

    // Get sparsity
    Sparsity s = Sparsity::compressed(m->sparsity);

    // Symbolic expression for A
    SX A = SX::sym("A", s);

    // BTF factorization
    const Sparsity::Btf& btf = s.btf();

    // Get the inverted column permutation
    std::vector<int> inv_colperm(btf.colperm.size());
    for (int k=0; k<btf.colperm.size(); ++k)
      inv_colperm[btf.colperm[k]] = k;

    // Get the inverted row permutation
    std::vector<int> inv_rowperm(btf.rowperm.size());
    for (int k=0; k<btf.rowperm.size(); ++k)
      inv_rowperm[btf.rowperm[k]] = k;

    // Permute the linear system
    SX Aperm = A(btf.rowperm, btf.colperm);

    // Generate the QR factorization function
    SX Q1, R1;
    qr(Aperm, Q1, R1);
    m->factorize = Function("QR_fact", {A}, {Q1, R1}, fopts_);
    m->alloc(m->factorize);

    // Symbolic expressions for solve function
    SX Q = SX::sym("Q", Q1.sparsity());
    SX R = SX::sym("R", R1.sparsity());
    SX b = SX::sym("b", s.size2(), 1);

    // Solve non-transposed
    // We have Pb' * Q * R * Px * x = b <=> x = Px' * inv(R) * Q' * Pb * b

    // Permute the right hand sides
    SX bperm = b(btf.rowperm, Slice());

    // Solve the factorized system
    SX xperm = SX::solve(R, mtimes(Q.T(), bperm));

    // Permute back the solution
    SX x = xperm(inv_colperm, Slice());

    // Generate the QR solve function
    vector<SX> solv_in = {Q, R, b};
    m->solve = Function("QR_solv", solv_in, {x}, fopts_);
    m->alloc(m->solve);

    // Solve transposed
    // We have (Pb' * Q * R * Px)' * x = b
    // <=> Px' * R' * Q' * Pb * x = b
    // <=> x = Pb' * Q * inv(R') * Px * b

    // Permute the right hand side
    bperm = b(btf.colperm, Slice());

    // Solve the factorized system
    xperm = mtimes(Q, SX::solve(R.T(), bperm));

    // Permute back the solution
    x = xperm(inv_rowperm, Slice());

    // Mofify the QR solve function
    m->solveT = Function("QR_solv_T", solv_in, {x}, fopts_);
    m->alloc(m->solveT);

    // Temporary storage
    m->w.resize(m->w.size() + s.size1());

    // Allocate storage for QR factorization
    m->q.resize(m->factorize.nnz_out(0));
    m->r.resize(m->factorize.nnz_out(1));
  }

  void SymbolicQr::factorize(void* mem, const double* A) const {
    auto m = static_cast<SymbolicQrMemory*>(mem);

    // Factorize
    fill_n(get_ptr(m->arg), m->factorize.n_in(), nullptr);
    m->arg[0] = A;
    fill_n(get_ptr(m->res), m->factorize.n_out(), nullptr);
    m->res[0] = get_ptr(m->q);
    m->res[1] = get_ptr(m->r);
    m->factorize(get_ptr(m->arg), get_ptr(m->res), get_ptr(m->iw), get_ptr(m->w));
  }

  void SymbolicQr::solve(void* mem, double* x, int nrhs, bool tr) const {
    auto m = static_cast<SymbolicQrMemory*>(mem);

    // Select solve function
    const Function& solv = tr ? m->solveT : m->solve;

    // Solve for all right hand sides
    fill_n(get_ptr(m->arg), solv.n_in(), nullptr);
    m->arg[0] = get_ptr(m->q);
    m->arg[1] = get_ptr(m->r);
    fill_n(get_ptr(m->res), solv.n_out(), nullptr);
    for (int i=0; i<nrhs; ++i) {
      copy_n(x, m->nrow(), get_ptr(m->w)); // Copy x to a temporary
      m->arg[2] = get_ptr(m->w);
      m->res[0] = x;
      solv(get_ptr(m->arg), get_ptr(m->res), get_ptr(m->iw), get_ptr(m->w)+m->nrow(), 0);
      x += m->nrow();
    }
  }

  void SymbolicQr::linsol_eval_sx(const SXElem** arg, SXElem** res, int* iw, SXElem* w, int mem,
                                 bool tr, int nrhs) {
    auto m = static_cast<SymbolicQrMemory*>(memory(mem));
    casadi_assert(arg[0]!=0);
    casadi_assert(arg[1]!=0);
    casadi_assert(res[0]!=0);

    // Get A and factorize it
    Sparsity sp = Sparsity::compressed(m->sparsity);
    SX A = SX::zeros(sp);
    copy(arg[1], arg[1]+A.nnz(), A->begin());
    vector<SX> v = m->factorize(A);

    // Select solve function
    Function& solv = tr ? m->solveT : m->solve;

    // Solve for every right hand side
    v.push_back(SX::zeros(A.size1()));
    const SXElem* a=arg[0];
    SXElem* r=res[0];
    for (int i=0; i<nrhs; ++i) {
      copy(a, a+v[2].nnz(), v[2]->begin());
      SX rr = solv(v).at(0);
      copy(rr->begin(), rr->end(), r);
      r += rr.nnz();
    }
  }

  void SymbolicQrMemory::alloc(const Function& f) {
    arg.resize(max(arg.size(), f.sz_arg()));
    res.resize(max(res.size(), f.sz_res()));
    iw.resize(max(iw.size(), f.sz_iw()));
    w.resize(max(w.size(), f.sz_w()));
  }

} // namespace casadi
