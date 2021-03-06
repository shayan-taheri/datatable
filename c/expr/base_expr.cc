//------------------------------------------------------------------------------
// Copyright 2018 H2O.ai
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//------------------------------------------------------------------------------
#include <memory>             // std::unique_ptr
#include <stdlib.h>
#include "datatable.h"
#include "datatablemodule.h"
#include "expr/base_expr.h"
#include "expr/py_expr.h"
#include "expr/workframe.h"

namespace dt {


//------------------------------------------------------------------------------
// base_expr
//------------------------------------------------------------------------------


base_expr::base_expr() {
  TRACK(this, sizeof(*this), "dt::base_expr");
}

base_expr::~base_expr() {
  UNTRACK(this);
}

bool base_expr::is_column_expr() const { return false; }

bool base_expr::is_negated_expr() const { return false; }

pexpr base_expr::get_negated_expr() { return pexpr(); }

size_t base_expr::get_col_index(const workframe&) { return size_t(-1); }



//------------------------------------------------------------------------------
// expr_column
//------------------------------------------------------------------------------

expr_column::expr_column(size_t dfid, const py::robj& col)
  : frame_id(dfid), col_id(size_t(-1)), col_selector(col) {}


size_t expr_column::get_frame_id() const noexcept {
  return frame_id;
}


bool expr_column::is_column_expr() const {
  return true;
}


size_t expr_column::get_col_index(const workframe& wf) {
  if (col_id == size_t(-1)) {
    if (frame_id >= wf.nframes()) {
      throw ValueError()
          << "Column expression references a non-existing join frame";
    }
    const DataTable* dt = wf.get_datatable(frame_id);
    if (col_selector.is_int()) {
      int64_t icolid = col_selector.to_int64_strict();
      int64_t incols = static_cast<int64_t>(dt->ncols);
      if (icolid < -incols || icolid >= incols) {
        throw ValueError() << "Column index " << icolid << " is invalid for "
            "a Frame with " << incols << " column" << (incols == 1? "" : "s");
      }
      if (icolid < 0) icolid += incols;
      col_id = static_cast<size_t>(icolid);
    }
    else if (col_selector.is_string()) {
      col_id = dt->xcolindex(col_selector);
    }
    xassert(col_id < dt->ncols);
  }
  return col_id;
}


SType expr_column::resolve(const workframe& wf) {
  size_t i = get_col_index(wf);
  const DataTable* dt = wf.get_datatable(frame_id);
  return dt->columns[i]->stype();
}


GroupbyMode expr_column::get_groupby_mode(const workframe& wf) const {
  return (frame_id == 0 &&
          wf.has_groupby() &&
          wf.get_by_node().has_group_column(col_id))? GroupbyMode::GtoONE
                                                    : GroupbyMode::GtoALL;
}


colptr expr_column::evaluate_eager(workframe& wf) {
  const DataTable* dt = wf.get_datatable(frame_id);
  const Column* rcol = dt->columns[col_id];
  const RowIndex& dt_ri = wf.get_rowindex(frame_id);
  const RowIndex& col_ri = rcol->rowindex();

  if (dt_ri) {
    return colptr(rcol->shallowcopy(wf._product(dt_ri, col_ri)));
  }
  else {
    return colptr(rcol->shallowcopy());
  }
}



//------------------------------------------------------------------------------
// expr_binaryop
//------------------------------------------------------------------------------

static std::vector<std::string> binop_names;
static std::unordered_map<size_t, SType> binop_rules;

static size_t id(biop opcode) {
  return static_cast<size_t>(opcode);
}
static size_t id(biop opcode, SType st1, SType st2) {
  return (static_cast<size_t>(opcode) << 16) +
         (static_cast<size_t>(st1) << 8) +
         (static_cast<size_t>(st2));
}

static void init_binops() {
  constexpr SType bool8 = SType::BOOL;
  constexpr SType int8  = SType::INT8;
  constexpr SType int16 = SType::INT16;
  constexpr SType int32 = SType::INT32;
  constexpr SType int64 = SType::INT64;
  constexpr SType flt32 = SType::FLOAT32;
  constexpr SType flt64 = SType::FLOAT64;
  constexpr SType str32 = SType::STR32;
  constexpr SType str64 = SType::STR64;

  using styvec = std::vector<SType>;
  styvec integer_stypes = {int8, int16, int32, int64};
  styvec numeric_stypes = {bool8, int8, int16, int32, int64, flt32, flt64};
  styvec string_types = {str32, str64};

  for (SType st1 : numeric_stypes) {
    for (SType st2 : numeric_stypes) {
      SType stm = std::max(st1, st2);
      binop_rules[id(biop::PLUS, st1, st2)] = stm;
      binop_rules[id(biop::MINUS, st1, st2)] = stm;
      binop_rules[id(biop::MULTIPLY, st1, st2)] = stm;
      binop_rules[id(biop::POWER, st1, st2)] = stm;
      binop_rules[id(biop::DIVIDE, st1, st2)] = flt64;
      binop_rules[id(biop::REL_EQ, st1, st2)] = bool8;
      binop_rules[id(biop::REL_NE, st1, st2)] = bool8;
      binop_rules[id(biop::REL_LT, st1, st2)] = bool8;
      binop_rules[id(biop::REL_GT, st1, st2)] = bool8;
      binop_rules[id(biop::REL_LE, st1, st2)] = bool8;
      binop_rules[id(biop::REL_GE, st1, st2)] = bool8;
    }
  }
  for (SType st1 : integer_stypes) {
    for (SType st2 : integer_stypes) {
      SType stm = std::max(st1, st2);
      binop_rules[id(biop::INT_DIVIDE, st1, st2)] = stm;
      binop_rules[id(biop::MODULO, st1, st2)] = stm;
      binop_rules[id(biop::LEFT_SHIFT, st1, st2)] = stm;
      binop_rules[id(biop::RIGHT_SHIFT, st1, st2)] = stm;
    }
  }
  for (SType st1 : string_types) {
    for (SType st2 : string_types) {
      binop_rules[id(biop::REL_EQ, st1, st2)] = bool8;
      binop_rules[id(biop::REL_NE, st1, st2)] = bool8;
    }
  }
  binop_rules[id(biop::LOGICAL_AND, bool8, bool8)] = bool8;
  binop_rules[id(biop::LOGICAL_OR, bool8, bool8)] = bool8;

  binop_names.resize(18);
  binop_names[id(biop::PLUS)] = "+";
  binop_names[id(biop::MINUS)] = "-";
  binop_names[id(biop::MULTIPLY)] = "*";
  binop_names[id(biop::DIVIDE)] = "/";
  binop_names[id(biop::INT_DIVIDE)] = "//";
  binop_names[id(biop::POWER)] = "**";
  binop_names[id(biop::MODULO)] = "%";
  binop_names[id(biop::LOGICAL_AND)] = "&";
  binop_names[id(biop::LOGICAL_OR)] = "|";
  binop_names[id(biop::LEFT_SHIFT)] = "<<";
  binop_names[id(biop::RIGHT_SHIFT)] = ">>";
  binop_names[id(biop::REL_EQ)] = "==";
  binop_names[id(biop::REL_NE)] = "!=";
  binop_names[id(biop::REL_GT)] = ">";
  binop_names[id(biop::REL_LT)] = "<";
  binop_names[id(biop::REL_GE)] = ">=";
  binop_names[id(biop::REL_LE)] = "<=";
}


class expr_binaryop : public base_expr {
  private:
    pexpr lhs;
    pexpr rhs;
    size_t binop_code;

  public:
    expr_binaryop(size_t opcode, pexpr&& l, pexpr&& r);
    SType resolve(const workframe& wf) override;
    GroupbyMode get_groupby_mode(const workframe&) const override;
    colptr evaluate_eager(workframe& wf) override;
};


expr_binaryop::expr_binaryop(size_t opcode, pexpr&& l, pexpr&& r)
  : lhs(std::move(l)), rhs(std::move(r)), binop_code(opcode) {}


SType expr_binaryop::resolve(const workframe& wf) {
  SType lhs_stype = lhs->resolve(wf);
  SType rhs_stype = rhs->resolve(wf);
  size_t triple = id(static_cast<biop>(binop_code), lhs_stype, rhs_stype);
  if (binop_rules.count(triple) == 0) {
    throw TypeError() << "Binary operator `" << binop_names[binop_code]
        << "` cannot be applied to columns with stypes `" << lhs_stype
        << "` and `" << rhs_stype << "`";
  }
  return binop_rules.at(triple);
}


GroupbyMode expr_binaryop::get_groupby_mode(const workframe& wf) const {
  auto lmode = static_cast<uint8_t>(lhs->get_groupby_mode(wf));
  auto rmode = static_cast<uint8_t>(rhs->get_groupby_mode(wf));
  return static_cast<GroupbyMode>(std::max(lmode, rmode));
}


colptr expr_binaryop::evaluate_eager(workframe& wf) {
  auto lhs_res = lhs->evaluate_eager(wf);
  auto rhs_res = rhs->evaluate_eager(wf);
  return colptr(expr::binaryop(binop_code, lhs_res.get(), rhs_res.get()));
}



//------------------------------------------------------------------------------
// expr_literal
//------------------------------------------------------------------------------

class expr_literal : public base_expr {
  private:
    colptr col;

  public:
    explicit expr_literal(const py::robj&);
    SType resolve(const workframe&) override;
    GroupbyMode get_groupby_mode(const workframe&) const override;
    colptr evaluate_eager(workframe&) override;
};


expr_literal::expr_literal(const py::robj& v) {
  py::olist lst(1);
  lst.set(0, v);
  col = colptr(Column::from_pylist(lst, 0));
}


SType expr_literal::resolve(const workframe&) {
  return col->stype();
}


GroupbyMode expr_literal::get_groupby_mode(const workframe&) const {
  return GroupbyMode::GtoONE;
}


colptr expr_literal::evaluate_eager(workframe&) {
  return colptr(col->shallowcopy());
}



//------------------------------------------------------------------------------
// expr_unaryop
//------------------------------------------------------------------------------

static std::vector<std::string> unop_names;
static std::unordered_map<size_t, SType> unop_rules;

static size_t id(unop opcode) {
  return static_cast<size_t>(opcode);
}
static size_t id(unop opcode, SType st1) {
  return (static_cast<size_t>(opcode) << 8) + static_cast<size_t>(st1);
}

static void init_unops() {
  constexpr SType bool8 = SType::BOOL;
  constexpr SType int8  = SType::INT8;
  constexpr SType int16 = SType::INT16;
  constexpr SType int32 = SType::INT32;
  constexpr SType int64 = SType::INT64;
  constexpr SType flt32 = SType::FLOAT32;
  constexpr SType flt64 = SType::FLOAT64;
  constexpr SType str32 = SType::STR32;
  constexpr SType str64 = SType::STR64;

  using styvec = std::vector<SType>;
  styvec integer_stypes = {int8, int16, int32, int64};
  styvec numeric_stypes = {bool8, int8, int16, int32, int64, flt32, flt64};
  styvec string_types = {str32, str64};
  styvec all_stypes = {bool8, int8, int16, int32, int64,
                       flt32, flt64, str32, str64};

  for (SType st : all_stypes) {
    unop_rules[id(unop::ISNA, st)] = bool8;
  }
  for (SType st : integer_stypes) {
    unop_rules[id(unop::INVERT, st)] = st;
  }
  for (SType st : numeric_stypes) {
    unop_rules[id(unop::MINUS, st)] = st;
    unop_rules[id(unop::PLUS, st)] = st;
    unop_rules[id(unop::ABS, st)] = st;
    unop_rules[id(unop::EXP, st)] = flt64;
    unop_rules[id(unop::LOGE, st)] = flt64;
    unop_rules[id(unop::LOG10, st)] = flt64;
  }
  unop_rules[id(unop::MINUS, bool8)] = int8;
  unop_rules[id(unop::PLUS, bool8)] = int8;
  unop_rules[id(unop::ABS, bool8)] = int8;
  unop_rules[id(unop::INVERT, bool8)] = bool8;
  unop_rules[id(unop::LEN, str32)] = int32;
  unop_rules[id(unop::LEN, str64)] = int64;

  unop_names.resize(1 + id(unop::LEN));
  unop_names[id(unop::ISNA)]   = "isna";
  unop_names[id(unop::MINUS)]  = "-";
  unop_names[id(unop::PLUS)]   = "+";
  unop_names[id(unop::INVERT)] = "~";
  unop_names[id(unop::ABS)]    = "abs";
  unop_names[id(unop::EXP)]    = "exp";
  unop_names[id(unop::LOGE)]   = "log";
  unop_names[id(unop::LOG10)]  = "log10";
  unop_names[id(unop::LEN)]    = "len";
}



class expr_unaryop : public base_expr {
  private:
    pexpr arg;
    unop unop_code;

  public:
    expr_unaryop(size_t opcode, pexpr a);
    bool is_negated_expr() const override;
    pexpr get_negated_expr() override;
    SType resolve(const workframe& wf) override;
    GroupbyMode get_groupby_mode(const workframe&) const override;
    colptr evaluate_eager(workframe& wf) override;
};


expr_unaryop::expr_unaryop(size_t opcode, pexpr a)
  : arg(std::move(a)), unop_code(static_cast<unop>(opcode)) {}


bool expr_unaryop::is_negated_expr() const {
  return unop_code == unop::MINUS;
}

pexpr expr_unaryop::get_negated_expr() {
  pexpr res;
  if (unop_code == unop::MINUS) {
    res = std::move(arg);
  }
  return res;
}


SType expr_unaryop::resolve(const workframe& wf) {
  SType arg_stype = arg->resolve(wf);
  size_t op_id = id(unop_code, arg_stype);
  if (unop_rules.count(op_id) == 0) {
    throw TypeError() << "Unary operator `"
        << unop_names[static_cast<size_t>(unop_code)]
        << "` cannot be applied to a column with stype `" << arg_stype << "`";
  }
  return unop_rules.at(op_id);
}


GroupbyMode expr_unaryop::get_groupby_mode(const workframe& wf) const {
  return arg->get_groupby_mode(wf);
}


colptr expr_unaryop::evaluate_eager(workframe& wf) {
  auto arg_res = arg->evaluate_eager(wf);
  return colptr(expr::unaryop(unop_code, arg_res.get()));
}




//------------------------------------------------------------------------------
// expr_cast
//------------------------------------------------------------------------------

class expr_cast : public base_expr {
  private:
    pexpr arg;
    SType stype;
    size_t : 56;

  public:
    expr_cast(pexpr&& a, SType s);
    SType resolve(const workframe& wf) override;
    GroupbyMode get_groupby_mode(const workframe&) const override;
    colptr evaluate_eager(workframe& wf) override;
};


expr_cast::expr_cast(pexpr&& a, SType s)
  : arg(std::move(a)), stype(s) {}


SType expr_cast::resolve(const workframe& wf) {
  (void) arg->resolve(wf);
  return stype;
}


GroupbyMode expr_cast::get_groupby_mode(const workframe& wf) const {
  return arg->get_groupby_mode(wf);
}


colptr expr_cast::evaluate_eager(workframe& wf) {
  auto arg_col = arg->evaluate_eager(wf);
  return colptr(arg_col->cast(stype));
}




//------------------------------------------------------------------------------
// expr_reduce_nullary
//------------------------------------------------------------------------------

class expr_reduce_nullary : public base_expr {
  private:
    size_t opcode;

  public:
    explicit expr_reduce_nullary(size_t op);
    SType resolve(const workframe& wf) override;
    GroupbyMode get_groupby_mode(const workframe&) const override;
    colptr evaluate_eager(workframe& wf) override;
};


expr_reduce_nullary::expr_reduce_nullary(size_t op) : opcode(op) {}


SType expr_reduce_nullary::resolve(const workframe&) {
  return SType::INT64;
}


GroupbyMode expr_reduce_nullary::get_groupby_mode(const workframe&) const {
  return GroupbyMode::GtoONE;
}


colptr expr_reduce_nullary::evaluate_eager(workframe& wf) {
  Column* res = nullptr;
  if (opcode == 0) {  // COUNT
    if (wf.has_groupby()) {
      const Groupby& grpby = wf.get_groupby();
      size_t ng = grpby.ngroups();
      const int32_t* offsets = grpby.offsets_r();
      res = Column::new_data_column(SType::INT32, ng);
      auto d_res = static_cast<int32_t*>(res->data_w());
      for (size_t i = 0; i < ng; ++i) {
        d_res[i] = offsets[i + 1] - offsets[i];
      }
    } else {
      res = Column::new_data_column(SType::INT64, 1);
      auto d_res = static_cast<int64_t*>(res->data_w());
      d_res[0] = static_cast<int64_t>(wf.nrows());
    }
  }
  return colptr(res);
}



};
//------------------------------------------------------------------------------
// py::base_expr
//------------------------------------------------------------------------------

py::PKArgs py::base_expr::Type::args___init__(
    1, 0, 0, true, false, {"opcode"}, "__init__", nullptr);

const char* py::base_expr::Type::classname() {
  return "base_expr";
}

const char* py::base_expr::Type::classdoc() {
  return "Internal expression object";
}


static void check_args_count(const std::vector<py::robj>& va, size_t n) {
  if (va.size() == n) return;
  throw TypeError() << "Expected " << n << " additional arguments, but "
      "received " << va.size();
}

static std::unique_ptr<dt::base_expr> to_base_expr(const py::robj& arg) {
  PyObject* v = arg.to_borrowed_ref();
  if (Py_TYPE(v) != &py::base_expr::Type::type) {
    throw TypeError()
        << "Expected a base_expr object, but got " << arg.typeobj();
  }
  auto vv = reinterpret_cast<py::base_expr*>(v);
  return vv->release();
}


void py::base_expr::m__init__(py::PKArgs& args) {
  expr = nullptr;

  size_t opcode = args[0].to_size_t();
  std::vector<py::robj> va;
  va.reserve(args.num_vararg_args());
  for (auto item : args.varargs()) va.push_back(std::move(item));

  switch (opcode) {
    case dt::exprCode::COL: {
      check_args_count(va, 2);
      expr = new dt::expr_column(va[0].to_size_t(), va[1]);
      break;
    }
    case dt::exprCode::BINOP: {
      check_args_count(va, 3);
      size_t binop_code = va[0].to_size_t();
      auto lhs = to_base_expr(va[1]);
      auto rhs = to_base_expr(va[2]);
      expr = new dt::expr_binaryop(binop_code, std::move(lhs), std::move(rhs));
      break;
    }
    case dt::exprCode::LITERAL: {
      check_args_count(va, 1);
      expr = new dt::expr_literal(va[0]);
      break;
    }
    case dt::exprCode::UNOP: {
      check_args_count(va, 2);
      size_t unop_code = va[0].to_size_t();
      auto arg = to_base_expr(va[1]);
      expr = new dt::expr_unaryop(unop_code, std::move(arg));
      break;
    }
    case dt::exprCode::CAST: {
      check_args_count(va, 2);
      auto arg = to_base_expr(va[0]);
      SType stype = static_cast<SType>(va[1].to_size_t());
      expr = new dt::expr_cast(std::move(arg), stype);
      break;
    }
    case dt::exprCode::UNREDUCE: {
      check_args_count(va, 2);
      size_t op = va[0].to_size_t();
      auto arg = to_base_expr(va[1]);
      expr = new expr::expr_reduce(std::move(arg), op);
      break;
    }
    case dt::exprCode::NUREDUCE: {
      check_args_count(va, 1);
      size_t op = va[0].to_size_t();
      xassert(op == 0);
      expr = new dt::expr_reduce_nullary(op);
      break;
    }
    case dt::exprCode::STRINGFN: {
      check_args_count(va, 3);
      size_t op = va[0].to_size_t();
      auto arg = to_base_expr(va[1]);
      oobj params = va[2];
      expr = dt::expr_string_fn(op, std::move(arg), params).release();
      break;
    }
  }
}

void py::base_expr::m__dealloc__() {
  delete expr;
  expr = nullptr;
}


std::unique_ptr<dt::base_expr> py::base_expr::release() {
  dt::base_expr* ptr = expr;
  expr = nullptr;
  return std::unique_ptr<dt::base_expr>(ptr);
}


void py::base_expr::Type::init_methods_and_getsets(
    py::ExtType<py::base_expr>::Methods&,
    py::ExtType<py::base_expr>::GetSetters&)
{
  dt::init_unops();
  dt::init_binops();
}


bool is_PyBaseExpr(const py::_obj& obj) {
  static auto BaseExprType = py::oobj::import("datatable.expr", "BaseExpr");
  return PyObject_IsInstance(obj.to_borrowed_ref(),
                             BaseExprType.to_borrowed_ref());
}
