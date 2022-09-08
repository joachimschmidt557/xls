// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/codegen/vast.h"

#include "absl/flags/flag.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/strip.h"
#include "xls/common/indent.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "re2/re2.h"

namespace xls {
namespace verilog {

namespace {

int64_t NumberOfNewlines(absl::string_view string) {
  int64_t number_of_newlines = 0;
  for (char c : string) {
    if (c == '\n') {
      ++number_of_newlines;
    }
  }
  return number_of_newlines;
}

void LineInfoStart(LineInfo* line_info, const VastNode* node) {
  if (line_info != nullptr) {
    line_info->Start(node);
  }
}

void LineInfoEnd(LineInfo* line_info, const VastNode* node) {
  if (line_info != nullptr) {
    line_info->End(node);
  }
}

void LineInfoIncrease(LineInfo* line_info, int64_t delta) {
  if (line_info != nullptr) {
    line_info->Increase(delta);
  }
}

}  // namespace

std::string PartialLineSpans::ToString() const {
  return absl::StrCat(
      "[",
      absl::StrJoin(completed_spans, ", ",
                    [](std::string* out, const LineSpan& line_span) {
                      return line_span.ToString();
                    }),
      hanging_start_line.has_value()
          ? absl::StrCat("; ", hanging_start_line.value())
          : "",
      "]");
}

void LineInfo::Start(const VastNode* node) {
  if (!spans_.contains(node)) {
    spans_[node];
  }
  XLS_CHECK(!spans_.at(node).hanging_start_line.has_value())
      << "LineInfoStart can't be called twice in a row on the same node!";
  spans_.at(node).hanging_start_line = current_line_number_;
}

void LineInfo::End(const VastNode* node) {
  XLS_CHECK(spans_.contains(node))
      << "LineInfoEnd called without corresponding LineInfoStart!";
  XLS_CHECK(spans_.at(node).hanging_start_line.has_value())
      << "LineInfoEnd can't be called twice in a row on the same node!";
  int64_t start_line = spans_.at(node).hanging_start_line.value();
  int64_t end_line = current_line_number_;
  spans_.at(node).completed_spans.push_back(LineSpan(start_line, end_line));
  spans_.at(node).hanging_start_line = std::nullopt;
}

void LineInfo::Increase(int64_t delta) { current_line_number_ += delta; }

std::optional<std::vector<LineSpan>> LineInfo::LookupNode(
    const VastNode* node) const {
  if (!spans_.contains(node)) {
    return std::nullopt;
  }
  if (spans_.at(node).hanging_start_line.has_value()) {
    return std::nullopt;
  }
  return spans_.at(node).completed_spans;
}

std::string SanitizeIdentifier(absl::string_view name) {
  if (name.empty()) {
    return "_";
  }
  // Numbers can appear anywhere in the identifier except the first
  // character. Handle this case by prefixing the sanitized name with an
  // underscore.
  std::string sanitized = absl::ascii_isdigit(name[0]) ? absl::StrCat("_", name)
                                                       : std::string(name);
  for (int i = 0; i < sanitized.size(); ++i) {
    if (!absl::ascii_isalnum(sanitized[i])) {
      sanitized[i] = '_';
    }
  }
  return sanitized;
}

std::string ToString(Direction direction) {
  switch (direction) {
    case Direction::kInput:
      return "input";
    case Direction::kOutput:
      return "output";
    default:
      return "<invalid direction>";
  }
}

std::string MacroRef::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(name_));
  LineInfoEnd(line_info, this);
  return absl::StrCat("`", name_);
}

std::string Include::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(path_));
  LineInfoEnd(line_info, this);
  return absl::StrFormat("`include \"%s\"", path_);
}

DataType* VerilogFile::BitVectorTypeNoScalar(int64_t bit_count,
                                             const SourceInfo& loc,
                                             bool is_signed) {
  return Make<DataType>(loc, PlainLiteral(bit_count, loc), is_signed);
}

DataType* VerilogFile::BitVectorType(int64_t bit_count, const SourceInfo& loc,
                                     bool is_signed) {
  XLS_CHECK_GT(bit_count, 0);
  if (bit_count == 1) {
    if (is_signed) {
      return Make<DataType>(loc, /*width=*/nullptr, /*is_signed=*/true);
    } else {
      return Make<DataType>(loc);
    }
  }
  return BitVectorTypeNoScalar(bit_count, loc, is_signed);
}

DataType* VerilogFile::PackedArrayType(int64_t element_bit_count,
                                       absl::Span<const int64_t> dims,
                                       const SourceInfo& loc, bool is_signed) {
  XLS_CHECK_GT(element_bit_count, 0);
  std::vector<Expression*> dim_exprs;
  for (int64_t d : dims) {
    dim_exprs.push_back(PlainLiteral(d, loc));
  }
  // For packed arrays we always use a bitvector (non-scalar) for the element
  // type when the element bit width is 1. For example, if element bit width is
  // one and dims is {42} we generate the following type:
  //   reg [0:0][41:0] foo;
  // If we emitted a scalar type, it would look like:
  //   reg [41:0] foo;
  // Which would generate invalid verilog if we index into an element
  // (e.g. foo[2][0]) because scalars are not indexable.
  return Make<DataType>(
      loc, PlainLiteral(element_bit_count, loc), /*packed_dims=*/dim_exprs,
      /*unpacked_dims=*/std::vector<Expression*>(), is_signed);
}

DataType* VerilogFile::UnpackedArrayType(int64_t element_bit_count,
                                         absl::Span<const int64_t> dims,
                                         const SourceInfo& loc,
                                         bool is_signed) {
  XLS_CHECK_GT(element_bit_count, 0);
  std::vector<Expression*> dim_exprs;
  for (int64_t d : dims) {
    dim_exprs.push_back(PlainLiteral(d, loc));
  }
  return Make<DataType>(
      loc,
      element_bit_count == 1 ? nullptr : PlainLiteral(element_bit_count, loc),
      /*packed_dims=*/std::vector<Expression*>(),
      /*unpacked_dims=*/dim_exprs, is_signed);
}

std::string VerilogFile::Emit(LineInfo* line_info) const {
  auto file_member_str = [=](const FileMember& member) -> std::string {
    return absl::visit(
        Visitor{[=](Include* m) -> std::string { return m->Emit(line_info); },
                [=](Module* m) -> std::string { return m->Emit(line_info); },
                [=](BlankLine* m) -> std::string { return m->Emit(line_info); },
                [=](Comment* m) -> std::string { return m->Emit(line_info); }},
        member);
  };

  std::string out;
  for (const FileMember& member : members_) {
    absl::StrAppend(&out, file_member_str(member), "\n");
    LineInfoIncrease(line_info, 1);
  }
  return out;
}

LocalParamItemRef* LocalParam::AddItem(absl::string_view name,
                                       Expression* value,
                                       const SourceInfo& loc) {
  items_.push_back(file()->Make<LocalParamItem>(loc, name, value));
  return file()->Make<LocalParamItemRef>(loc, items_.back());
}

CaseArm::CaseArm(CaseLabel label, VerilogFile* file, const SourceInfo& loc)
    : VastNode(file, loc),
      label_(label),
      statements_(file->Make<StatementBlock>(loc)) {}

std::string CaseArm::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::visit(
      Visitor{[=](Expression* named) { return named->Emit(line_info); },
              [](DefaultSentinel) { return std::string("default"); }},
      label_);
  LineInfoEnd(line_info, this);
  return result;
}

std::string StatementBlock::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  // TODO(meheff): We can probably be smarter about optionally emitting the
  // begin/end.
  if (statements_.empty()) {
    LineInfoEnd(line_info, this);
    return "begin end";
  }
  std::string result = "begin\n";
  LineInfoIncrease(line_info, 1);
  std::vector<std::string> lines;
  for (const auto& statement : statements_) {
    lines.push_back(statement->Emit(line_info));
    LineInfoIncrease(line_info, 1);
  }
  absl::StrAppend(&result, Indent(absl::StrJoin(lines, "\n")), "\nend");
  LineInfoEnd(line_info, this);
  return result;
}

Port Port::FromProto(const PortProto& proto, VerilogFile* f) {
  Port port;
  port.direction = proto.direction() == DIRECTION_INPUT ? Direction::kInput
                                                        : Direction::kOutput;
  port.wire = f->Make<WireDef>(SourceInfo(), proto.name(),
                               f->BitVectorType(proto.width(), SourceInfo()));
  return port;
}

std::string Port::ToString() const {
  return absl::StrFormat("Port(dir=%s, name=\"%s\")",
                         verilog::ToString(direction), name());
}

absl::StatusOr<PortProto> Port::ToProto() const {
  PortProto proto;
  proto.set_direction(direction == Direction::kInput ? DIRECTION_INPUT
                                                     : DIRECTION_OUTPUT);
  proto.set_name(wire->GetName());
  XLS_ASSIGN_OR_RETURN(int64_t width, wire->data_type()->FlatBitCountAsInt64());
  proto.set_width(width);
  return proto;
}

VerilogFunction::VerilogFunction(absl::string_view name, DataType* result_type,
                                 VerilogFile* file, const SourceInfo& loc)
    : VastNode(file, loc),
      name_(name),
      return_value_def_(file->Make<RegDef>(loc, name, result_type)),
      statement_block_(file->Make<StatementBlock>(loc)) {}

LogicRef* VerilogFunction::AddArgument(absl::string_view name, DataType* type,
                                       const SourceInfo& loc) {
  argument_defs_.push_back(file()->Make<RegDef>(loc, name, type));
  return file()->Make<LogicRef>(loc, argument_defs_.back());
}

LogicRef* VerilogFunction::return_value_ref() {
  return file()->Make<LogicRef>(return_value_def_->loc(), return_value_def_);
}

std::string VerilogFunction::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string return_type =
      return_value_def_->data_type()->EmitWithIdentifier(line_info, name());
  std::string parameters =
      absl::StrJoin(argument_defs_, ", ", [=](std::string* out, RegDef* d) {
        absl::StrAppend(out, "input ", d->EmitNoSemi(line_info));
      });
  LineInfoIncrease(line_info, 1);
  std::vector<std::string> lines;
  for (RegDef* reg_def : block_reg_defs_) {
    lines.push_back(reg_def->Emit(line_info));
    LineInfoIncrease(line_info, 1);
  }
  lines.push_back(statement_block_->Emit(line_info));
  LineInfoIncrease(line_info, 1);
  LineInfoEnd(line_info, this);
  return absl::StrCat(
      absl::StrFormat("function automatic%s (%s);\n", return_type, parameters),
      Indent(absl::StrJoin(lines, "\n")), "\nendfunction");
}

std::string VerilogFunctionCall::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrFormat(
      "%s(%s)", func_->name(),
      absl::StrJoin(args_, ", ", [=](std::string* out, Expression* e) {
        absl::StrAppend(out, e->Emit(line_info));
      }));
  LineInfoEnd(line_info, this);
  return result;
}

LogicRef* Module::AddPortDef(Direction direction, Def* def,
                             const SourceInfo& loc) {
  ports_.push_back(Port{direction, def});
  return file()->Make<LogicRef>(loc, def);
}

LogicRef* Module::AddInput(absl::string_view name, DataType* type,
                           const SourceInfo& loc) {
  return AddPortDef(Direction::kInput,
                    file()->Make<WireDef>(loc, name, std::move(type)), loc);
}

LogicRef* Module::AddOutput(absl::string_view name, DataType* type,
                            const SourceInfo& loc) {
  return AddPortDef(Direction::kOutput,
                    file()->Make<WireDef>(loc, name, std::move(type)), loc);
}

LogicRef* Module::AddReg(absl::string_view name, DataType* type,
                         const SourceInfo& loc, Expression* init,
                         ModuleSection* section) {
  if (section == nullptr) {
    section = &top_;
  }
  return file()->Make<LogicRef>(
      loc, section->Add<RegDef>(loc, name, std::move(type), init));
}

LogicRef* Module::AddWire(absl::string_view name, DataType* type,
                          const SourceInfo& loc, ModuleSection* section) {
  if (section == nullptr) {
    section = &top_;
  }
  return file()->Make<LogicRef>(
      loc, section->Add<WireDef>(loc, name, std::move(type)));
}

ParameterRef* Module::AddParameter(absl::string_view name, Expression* rhs,
                                   const SourceInfo& loc) {
  Parameter* param = AddModuleMember(file()->Make<Parameter>(loc, name, rhs));
  return file()->Make<ParameterRef>(loc, param);
}

void Module::AddAttribute(std::string name) {
  attributes_.push_back(name);
}

Literal* Expression::AsLiteralOrDie() {
  XLS_CHECK(IsLiteral());
  return static_cast<Literal*>(this);
}

IndexableExpression* Expression::AsIndexableExpressionOrDie() {
  XLS_CHECK(IsIndexableExpression());
  return static_cast<IndexableExpression*>(this);
}

Unary* Expression::AsUnaryOrDie() {
  XLS_CHECK(IsUnary());
  return static_cast<Unary*>(this);
}

LogicRef* Expression::AsLogicRefOrDie() {
  XLS_CHECK(IsLogicRef());
  return static_cast<LogicRef*>(this);
}

std::string XSentinel::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%d'dx", width_);
}

static void FourValueFormatter(std::string* out, FourValueBit value) {
  char value_as_char;
  switch (value) {
    case FourValueBit::kZero:
      value_as_char = '0';
      break;
    case FourValueBit::kOne:
      value_as_char = '1';
      break;
    case FourValueBit::kUnknown:
      value_as_char = 'X';
      break;
    case FourValueBit::kHighZ:
      value_as_char = '?';
      break;
  }
  out->push_back(value_as_char);
}

std::string FourValueBinaryLiteral::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoEnd(line_info, this);
  return absl::StrCat(bits_.size(), "'b",
                      absl::StrJoin(bits_, "", FourValueFormatter));
}

// Returns a string representation of the given expression minus one.
static std::string WidthToLimit(LineInfo* line_info, Expression* expr) {
  if (expr->IsLiteral()) {
    // If the expression is a literal, then we can emit the value - 1 directly.
    uint64_t value = expr->AsLiteralOrDie()->bits().ToUint64().value();
    return absl::StrCat(value - 1);
  }
  Literal* one = expr->file()->PlainLiteral(1, expr->loc());
  Expression* width_minus_one = expr->file()->Sub(expr, one, expr->loc());
  return width_minus_one->Emit(line_info);
}

std::string DataType::EmitWithIdentifier(LineInfo* line_info,
                                         absl::string_view identifier) const {
  LineInfoStart(line_info, this);
  std::string result = is_signed_ ? " signed" : "";
  if (width_ != nullptr) {
    absl::StrAppendFormat(&result, " [%s:0]", WidthToLimit(line_info, width()));
  }
  for (Expression* dim : packed_dims()) {
    absl::StrAppendFormat(&result, "[%s:0]", WidthToLimit(line_info, dim));
  }
  absl::StrAppend(&result, " ", identifier);
  for (Expression* dim : unpacked_dims()) {
    // In SystemVerilog unpacked arrays can be specified using only the size
    // rather than a range.
    if (file()->use_system_verilog()) {
      absl::StrAppendFormat(&result, "[%s]", dim->Emit(line_info));
    } else {
      absl::StrAppendFormat(&result, "[0:%s]", WidthToLimit(line_info, dim));
    }
  }
  LineInfoEnd(line_info, this);
  return result;
}

absl::StatusOr<int64_t> DataType::WidthAsInt64() const {
  if (width() == nullptr) {
    // No width indicates a single-bit signal.
    return 1;
  }

  if (!width()->IsLiteral()) {
    return absl::FailedPreconditionError("Width is not a literal: " +
                                         width()->Emit(nullptr));
  }
  return width()->AsLiteralOrDie()->bits().ToUint64();
}

absl::StatusOr<int64_t> DataType::FlatBitCountAsInt64() const {
  XLS_ASSIGN_OR_RETURN(int64_t bit_count, WidthAsInt64());
  for (Expression* dim : packed_dims()) {
    if (!dim->IsLiteral()) {
      return absl::FailedPreconditionError(
          "Packed dimension is not a literal:" + dim->Emit(nullptr));
    }
    XLS_ASSIGN_OR_RETURN(int64_t dim_size,
                         dim->AsLiteralOrDie()->bits().ToUint64());
    bit_count = bit_count * dim_size;
  }
  for (Expression* dim : unpacked_dims()) {
    if (!dim->IsLiteral()) {
      return absl::FailedPreconditionError(
          "Unpacked dimension is not a literal:" + dim->Emit(nullptr));
    }
    XLS_ASSIGN_OR_RETURN(int64_t dim_size,
                         dim->AsLiteralOrDie()->bits().ToUint64());
    bit_count = bit_count * dim_size;
  }
  return bit_count;
}

std::string Def::Emit(LineInfo* line_info) const {
  return EmitNoSemi(line_info) + ";";
}

std::string Def::EmitNoSemi(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string kind_str;
  switch (data_kind()) {
    case DataKind::kReg:
      kind_str = "reg";
      break;
    case DataKind::kWire:
      kind_str = "wire";
      break;
    case DataKind::kLogic:
      kind_str = "logic";
      break;
  }
  std::string result = absl::StrCat(
      kind_str, data_type()->EmitWithIdentifier(line_info, GetName()));
  LineInfoEnd(line_info, this);
  return result;
}

std::string RegDef::Emit(LineInfo* line_info) const {
  std::string result = Def::EmitNoSemi(line_info);
  if (init_ != nullptr) {
    absl::StrAppend(&result, " = ", init_->Emit(line_info));
  }
  absl::StrAppend(&result, ";");
  return result;
}

namespace {

// "Match" statement for emitting a ModuleMember.
std::string EmitModuleMember(LineInfo* line_info, const ModuleMember& member) {
  return absl::visit(
      Visitor{[=](Def* d) { return d->Emit(line_info); },
              [=](LocalParam* p) { return p->Emit(line_info); },
              [=](Parameter* p) { return p->Emit(line_info); },
              [=](Instantiation* i) { return i->Emit(line_info); },
              [=](ContinuousAssignment* c) { return c->Emit(line_info); },
              [=](Comment* c) { return c->Emit(line_info); },
              [=](BlankLine* b) { return b->Emit(line_info); },
              [=](InlineVerilogStatement* s) { return s->Emit(line_info); },
              [=](StructuredProcedure* sp) { return sp->Emit(line_info); },
              [=](AlwaysComb* ac) { return ac->Emit(line_info); },
              [=](AlwaysFf* af) { return af->Emit(line_info); },
              [=](AlwaysFlop* af) { return af->Emit(line_info); },
              [=](VerilogFunction* f) { return f->Emit(line_info); },
              [=](Cover* c) { return c->Emit(line_info); },
              [=](ModuleSection* s) { return s->Emit(line_info); }},
      member);
}

}  // namespace

std::string ModuleSection::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::vector<std::string> elements;
  for (const ModuleMember& member : members_) {
    if (absl::holds_alternative<ModuleSection*>(member)) {
      if (absl::get<ModuleSection*>(member)->members_.empty()) {
        continue;
      }
    }
    elements.push_back(EmitModuleMember(line_info, member));
    LineInfoIncrease(line_info, 1);
  }
  if (!elements.empty()) {
    LineInfoIncrease(line_info, -1);
  }
  LineInfoEnd(line_info, this);
  return absl::StrJoin(elements, "\n");
}

std::string ContinuousAssignment::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string lhs = lhs_->Emit(line_info);
  std::string rhs = rhs_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("assign %s = %s;", lhs, rhs);
}

std::string Comment::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(text_));
  LineInfoEnd(line_info, this);
  return absl::StrCat("// ", absl::StrReplaceAll(text_, {{"\n", "\n// "}}));
}

std::string InlineVerilogStatement::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(text_));
  LineInfoEnd(line_info, this);
  return text_;
}

std::string InlineVerilogRef::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(name_));
  LineInfoEnd(line_info, this);
  return name_;
}

std::string Assert::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  // The $fatal statement takes finish_number as the first argument which is a
  // value in the set {0, 1, 2}. This value "sets the level of diagnostic
  // information reported by the tool" (from IEEE Std 1800-2017).
  //
  // XLS emits asserts taking combinational inputs, so a deferred
  // immediate assertion is used.
  constexpr int64_t kFinishNumber = 0;
  std::string result = absl::StrFormat(
      "assert #0 (%s) else $fatal(%d%s);", condition_->Emit(line_info),
      kFinishNumber,
      error_message_.empty() ? ""
                             : absl::StrFormat(", \"%s\"", error_message_));
  LineInfoEnd(line_info, this);
  return result;
}

std::string Cover::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  // Coverpoints don't work without clock sources. Don't emit them in that case.
  LineInfoIncrease(line_info, NumberOfNewlines(label_));
  std::string clock = clk_->Emit(line_info);
  std::string condition = condition_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s: cover property (@(posedge %s) %s);", label_,
                         clock, condition);
}

std::string SystemTaskCall::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  if (args_.has_value()) {
    std::string result = absl::StrFormat(
        "$%s(%s);", name_,
        absl::StrJoin(*args_, ", ", [=](std::string* out, Expression* e) {
          absl::StrAppend(out, e->Emit(line_info));
        }));
    LineInfoEnd(line_info, this);
    return result;
  } else {
    LineInfoEnd(line_info, this);
    return absl::StrFormat("$%s;", name_);
  }
}

std::string SystemFunctionCall::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  if (args_.has_value()) {
    LineInfoIncrease(line_info, NumberOfNewlines(name_));
    std::string arg_list =
        absl::StrJoin(*args_, ", ", [=](std::string* out, Expression* e) {
          absl::StrAppend(out, e->Emit(line_info));
        });
    LineInfoEnd(line_info, this);
    return absl::StrFormat("$%s(%s)", name_, arg_list);
  } else {
    LineInfoIncrease(line_info, NumberOfNewlines(name_));
    LineInfoEnd(line_info, this);
    return absl::StrFormat("$%s", name_);
  }
}

std::string Module::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = "";
  if (!attributes_.empty()) {
    auto joined = absl::StrJoin(attributes_, ", ");
    absl::StrAppend(&result, "(* ", joined, " *)\n");
    LineInfoIncrease(line_info, 1);
  }
  absl::StrAppend(&result, "module ", name_);
  if (ports_.empty()) {
    absl::StrAppend(&result, ";\n");
    LineInfoIncrease(line_info, 1);
  } else {
    absl::StrAppend(&result, "(\n  ");
    LineInfoIncrease(line_info, 1);
    absl::StrAppend(
        &result,
        absl::StrJoin(ports_, ",\n  ", [=](std::string* out, const Port& port) {
          absl::StrAppendFormat(out, "%s %s", ToString(port.direction),
                                port.wire->EmitNoSemi(line_info));
          LineInfoIncrease(line_info, 1);
        }));
    absl::StrAppend(&result, "\n);\n");
    LineInfoIncrease(line_info, 1);
  }
  absl::StrAppend(&result, Indent(top_.Emit(line_info)), "\n");
  LineInfoIncrease(line_info, 1);
  absl::StrAppend(&result, "endmodule");
  LineInfoEnd(line_info, this);
  return result;
}

std::string Literal::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoEnd(line_info, this);
  if (format_ == FormatPreference::kDefault) {
    XLS_CHECK_LE(bits_.bit_count(), 32);
    return absl::StrFormat("%s",
                           bits_.ToString(FormatPreference::kUnsignedDecimal));
  }
  if (format_ == FormatPreference::kUnsignedDecimal) {
    std::string prefix;
    if (emit_bit_count_) {
      prefix = absl::StrFormat("%d'd", bits_.bit_count());
    }
    return absl::StrFormat("%s%s", prefix,
                           bits_.ToString(FormatPreference::kUnsignedDecimal));
  }
  if (format_ == FormatPreference::kBinary) {
    return absl::StrFormat(
        "%d'b%s", bits_.bit_count(),
        bits_.ToRawDigits(format_, /*emit_leading_zeros=*/true));
  }
  XLS_CHECK_EQ(format_, FormatPreference::kHex);
  return absl::StrFormat(
      "%d'h%s", bits_.bit_count(),
      bits_.ToRawDigits(FormatPreference::kHex, /*emit_leading_zeros=*/true));
}

bool Literal::IsLiteralWithValue(int64_t target) const {
  // VAST Literals are always unsigned. Signed literal values are created by
  // casting a VAST Literal to a signed type.
  if (target < 0) {
    return false;
  }
  if (!bits().FitsInUint64()) {
    return false;
  }
  return bits().ToUint64().value() == target;
}

// TODO(meheff): Escape string.
std::string QuotedString::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(str_));
  LineInfoEnd(line_info, this);
  return absl::StrFormat("\"%s\"", str_);
}

static bool IsScalarLogicRef(IndexableExpression* expr) {
  auto* logic_ref = dynamic_cast<LogicRef*>(expr);
  return logic_ref != nullptr && logic_ref->def()->data_type()->IsScalar();
}

std::string Slice::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  if (IsScalarLogicRef(subject_)) {
    // If subject is scalar (no width given in declaration) then avoid slicing
    // as this is invalid Verilog. The only valid hi/lo values are zero.
    // TODO(https://github.com/google/xls/issues/43): Avoid this special case
    // and perform the equivalent logic at a higher abstraction level than VAST.
    XLS_CHECK(hi_->IsLiteralWithValue(0)) << hi_->Emit(nullptr);
    XLS_CHECK(lo_->IsLiteralWithValue(0)) << lo_->Emit(nullptr);
    std::string result = subject_->Emit(line_info);
    LineInfoEnd(line_info, this);
    return result;
  }
  std::string subject = subject_->Emit(line_info);
  std::string hi = hi_->Emit(line_info);
  std::string lo = lo_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s[%s:%s]", subject, hi, lo);
}

std::string PartSelect::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string subject = subject_->Emit(line_info);
  std::string start = start_->Emit(line_info);
  std::string width = width_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s[%s +: %s]", subject, start, width);
}

std::string Index::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  if (IsScalarLogicRef(subject_)) {
    // If subject is scalar (no width given in declaration) then avoid indexing
    // as this is invalid Verilog. The only valid index values are zero.
    // TODO(https://github.com/google/xls/issues/43): Avoid this special case
    // and perform the equivalent logic at a higher abstraction level than VAST.
    XLS_CHECK(index_->IsLiteralWithValue(0)) << absl::StreamFormat(
        "%s[%s]", subject_->Emit(nullptr), index_->Emit(nullptr));
    std::string result = subject_->Emit(line_info);
    LineInfoEnd(line_info, this);
    return result;
  }
  std::string subject = subject_->Emit(line_info);
  std::string index = index_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s[%s]", subject, index);
}

// Returns the given string wrappend in parentheses.
static std::string ParenWrap(absl::string_view s) {
  return absl::StrFormat("(%s)", s);
}

std::string Ternary::Emit(LineInfo* line_info) const {
  auto maybe_paren_wrap = [this, line_info](Expression* e) {
    if (e->precedence() <= precedence()) {
      return ParenWrap(e->Emit(line_info));
    }
    return e->Emit(line_info);
  };
  LineInfoStart(line_info, this);
  std::string result = absl::StrFormat("%s ? %s : %s", maybe_paren_wrap(test_),
                                       maybe_paren_wrap(consequent_),
                                       maybe_paren_wrap(alternate_));
  LineInfoEnd(line_info, this);
  return result;
}

std::string Parameter::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(name_));
  std::string result =
      absl::StrFormat("parameter %s = %s;", name_, rhs_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string LocalParamItem::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(name_));
  std::string result = absl::StrFormat("%s = %s", name_, rhs_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string LocalParam::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = "localparam";
  if (items_.size() == 1) {
    absl::StrAppend(&result, " ", items_[0]->Emit(line_info), ";");
    LineInfoEnd(line_info, this);
    return result;
  }
  absl::StrAppend(&result, "\n  ");
  LineInfoIncrease(line_info, 1);
  auto append_item = [=](std::string* out, LocalParamItem* item) {
    absl::StrAppend(out, item->Emit(line_info));
    LineInfoIncrease(line_info, 1);
  };
  absl::StrAppend(&result, absl::StrJoin(items_, ",\n  ", append_item), ";");
  if (items_.size() > 1) {
    // StrJoin adds a fencepost number of newlines, so we need to subtract 1
    // to get the total number correct.
    LineInfoIncrease(line_info, -1);
  }
  LineInfoEnd(line_info, this);
  return result;
}

std::string BinaryInfix::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  auto is_unary_reduction = [](Expression* e) {
    return e->IsUnary() && e->AsUnaryOrDie()->IsReduction();
  };

  // Equal precedence operators are evaluated left-to-right so LHS only needs to
  // be wrapped if its precedence is strictly less than this operators. The RHS,
  // however, must be wrapped if its less than or equal precedence. Unary
  // reduction operations should be wrapped in parenthesis unconditionally
  // because some consumers of verilog emit warnings/errors for this
  // error-prone construct (e.g., `|x || |y`)
  std::string lhs_string =
      (lhs_->precedence() < precedence() || is_unary_reduction(lhs_))
          ? ParenWrap(lhs_->Emit(line_info))
          : lhs_->Emit(line_info);
  std::string rhs_string =
      (rhs_->precedence() <= precedence() || is_unary_reduction(rhs_))
          ? ParenWrap(rhs_->Emit(line_info))
          : rhs_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s %s %s", lhs_string, op_, rhs_string);
}

std::string Concat::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result;
  if (replication_ != nullptr) {
    absl::StrAppend(&result, "{", replication_->Emit(line_info));
  }
  absl::StrAppendFormat(
      &result, "{%s}",
      absl::StrJoin(args_, ", ", [=](std::string* out, Expression* e) {
        absl::StrAppend(out, e->Emit(line_info));
      }));
  if (replication_ != nullptr) {
    absl::StrAppend(&result, "}");
  }
  LineInfoEnd(line_info, this);
  return result;
}

std::string ArrayAssignmentPattern::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrFormat(
      "'{%s}", absl::StrJoin(args_, ", ", [=](std::string* out, Expression* e) {
        absl::StrAppend(out, e->Emit(line_info));
      }));
  LineInfoEnd(line_info, this);
  return result;
}

std::string Unary::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  // Nested unary ops should be wrapped in parentheses as this is required by
  // some consumers of Verilog.
  std::string result =
      absl::StrFormat("%s%s", op_,
                      ((arg_->precedence() < precedence()) || arg_->IsUnary())
                          ? ParenWrap(arg_->Emit(line_info))
                          : arg_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

StatementBlock* Case::AddCaseArm(CaseLabel label) {
  arms_.push_back(file()->Make<CaseArm>(SourceInfo(), label));
  return arms_.back()->statements();
}

static std::string CaseTypeToString(CaseType case_type) {
  std::string keyword;
  switch (case_type.keyword) {
    case CaseKeyword::kCase:
      keyword = "case";
      break;
    case CaseKeyword::kCasez:
      keyword = "casez";
      break;
    default:
      XLS_LOG(FATAL) << "Unexpected CaseKeyword with value "
                     << static_cast<int>(case_type.keyword);
  }

  if (case_type.modifier.has_value()) {
    std::string modifier;
    switch (*case_type.modifier) {
      case CaseModifier::kUnique:
        modifier = "unique";
        break;
      default:
        XLS_LOG(FATAL) << "Unexpected CaseModifier with value "
                       << static_cast<int>(*case_type.modifier);
    }
    return absl::StrCat(modifier, " ", keyword);
  }
  return keyword;
}

std::string Case::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrFormat(
      "%s (%s)\n", CaseTypeToString(case_type_), subject_->Emit(line_info));
  LineInfoIncrease(line_info, 1);
  for (auto& arm : arms_) {
    std::string arm_string = arm->Emit(line_info);
    std::string stmts_string = arm->statements()->Emit(line_info);
    absl::StrAppend(&result,
                    Indent(absl::StrFormat("%s: %s", arm_string, stmts_string)),
                    "\n");
    LineInfoIncrease(line_info, 1);
  }
  absl::StrAppend(&result, "endcase");
  LineInfoEnd(line_info, this);
  return result;
}

Conditional::Conditional(Expression* condition, VerilogFile* file,
                         const SourceInfo& loc)
    : Statement(file, loc),
      condition_(condition),
      consequent_(file->Make<StatementBlock>(SourceInfo())) {}

StatementBlock* Conditional::AddAlternate(Expression* condition) {
  // The conditional must not have been previously closed with an unconditional
  // alternate ("else").
  XLS_CHECK(alternates_.empty() || alternates_.back().first != nullptr);
  alternates_.push_back(
      {condition, file()->Make<StatementBlock>(SourceInfo())});
  return alternates_.back().second;
}

std::string Conditional::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result;
  std::string cond = condition_->Emit(line_info);
  std::string conseq = consequent()->Emit(line_info);
  absl::StrAppendFormat(&result, "if (%s) %s", cond, conseq);
  for (auto& alternate : alternates_) {
    absl::StrAppend(&result, " else ");
    if (alternate.first != nullptr) {
      absl::StrAppendFormat(&result, "if (%s) ",
                            alternate.first->Emit(line_info));
    }
    absl::StrAppend(&result, alternate.second->Emit(line_info));
  }
  LineInfoEnd(line_info, this);
  return result;
}

WhileStatement::WhileStatement(Expression* condition, VerilogFile* file,
                               const SourceInfo& loc)
    : Statement(file, loc),
      condition_(condition),
      statements_(file->Make<StatementBlock>(loc)) {}

std::string WhileStatement::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string condition = condition_->Emit(line_info);
  std::string stmts = statements()->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("while (%s) %s", condition, stmts);
}

std::string RepeatStatement::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string repeat_count = repeat_count_->Emit(line_info);
  std::string statement = statement_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("repeat (%s) %s;", repeat_count, statement);
}

std::string EventControl::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result =
      absl::StrFormat("@(%s);", event_expression_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string PosEdge::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result =
      absl::StrFormat("posedge %s", expression_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string NegEdge::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result =
      absl::StrFormat("negedge %s", expression_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string DelayStatement::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string delay_str = delay_->precedence() < Expression::kMaxPrecedence
                              ? ParenWrap(delay_->Emit(line_info))
                              : delay_->Emit(line_info);
  if (delayed_statement_ != nullptr) {
    std::string result = absl::StrFormat("#%s %s", delay_str,
                                         delayed_statement_->Emit(line_info));
    LineInfoEnd(line_info, this);
    return result;
  }
  LineInfoEnd(line_info, this);
  return absl::StrFormat("#%s;", delay_str);
}

std::string WaitStatement::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrFormat("wait(%s);", event_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string Forever::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrCat("forever ", statement_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string BlockingAssignment::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string lhs = lhs_->Emit(line_info);
  std::string rhs = rhs_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s = %s;", lhs, rhs);
}

std::string NonblockingAssignment::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string lhs = lhs_->Emit(line_info);
  std::string rhs = rhs_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s <= %s;", lhs, rhs);
}

StructuredProcedure::StructuredProcedure(VerilogFile* file,
                                         const SourceInfo& loc)
    : VastNode(file, loc), statements_(file->Make<StatementBlock>(loc)) {}

namespace {

std::string EmitSensitivityListElement(LineInfo* line_info,
                                       const SensitivityListElement& element) {
  return absl::visit(
      Visitor{[](ImplicitEventExpression e) -> std::string { return "*"; },
              [=](PosEdge* p) -> std::string { return p->Emit(line_info); },
              [=](NegEdge* n) -> std::string { return n->Emit(line_info); }},
      element);
}

}  // namespace

std::string AlwaysBase::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(name()));
  std::string sensitivity_list = absl::StrJoin(
      sensitivity_list_, " or ",
      [=](std::string* out, const SensitivityListElement& e) {
        absl::StrAppend(out, EmitSensitivityListElement(line_info, e));
      });
  std::string statements = statements_->Emit(line_info);
  LineInfoEnd(line_info, this);
  return absl::StrFormat("%s @ (%s) %s", name(), sensitivity_list, statements);
}

std::string AlwaysComb::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  LineInfoIncrease(line_info, NumberOfNewlines(name()));
  std::string result =
      absl::StrFormat("%s %s", name(), statements_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string Initial::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrCat("initial ", statements_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

AlwaysFlop::AlwaysFlop(LogicRef* clk, Reset rst, VerilogFile* file,
                       const SourceInfo& loc)
    : VastNode(file, loc),
      clk_(clk),
      rst_(rst),
      top_block_(file->Make<StatementBlock>(loc)) {
  // Reset signal specified. Construct conditional which switches the reset
  // signal.
  Expression* rst_condition;
  if (rst_->active_low) {
    rst_condition = file->LogicalNot(rst_->signal, loc);
  } else {
    rst_condition = rst_->signal;
  }
  Conditional* conditional = top_block_->Add<Conditional>(loc, rst_condition);
  reset_block_ = conditional->consequent();
  assignment_block_ = conditional->AddAlternate();
}

AlwaysFlop::AlwaysFlop(LogicRef* clk, VerilogFile* file, const SourceInfo& loc)
    : VastNode(file, loc),
      clk_(clk),
      top_block_(file->Make<StatementBlock>(loc)) {
  // No reset signal specified.
  reset_block_ = nullptr;
  assignment_block_ = top_block_;
}

void AlwaysFlop::AddRegister(LogicRef* reg, Expression* reg_next,
                             const SourceInfo& loc, Expression* reset_value) {
  if (reset_value != nullptr) {
    XLS_CHECK(reset_block_ != nullptr);
    reset_block_->Add<NonblockingAssignment>(loc, reg, reset_value);
  }
  assignment_block_->Add<NonblockingAssignment>(loc, reg, reg_next);
}

std::string AlwaysFlop::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result;
  std::string sensitivity_list =
      absl::StrCat("posedge ", clk_->Emit(line_info));
  if (rst_.has_value() && rst_->asynchronous) {
    absl::StrAppendFormat(&sensitivity_list, " or %s %s",
                          (rst_->active_low ? "negedge" : "posedge"),
                          rst_->signal->Emit(line_info));
  }
  absl::StrAppendFormat(&result, "always @ (%s) %s", sensitivity_list,
                        top_block_->Emit(line_info));
  LineInfoEnd(line_info, this);
  return result;
}

std::string Instantiation::Emit(LineInfo* line_info) const {
  LineInfoStart(line_info, this);
  std::string result = absl::StrCat(module_name_, " ");
  LineInfoIncrease(line_info, NumberOfNewlines(module_name_));
  auto append_connection = [=](std::string* out, const Connection& parameter) {
    absl::StrAppendFormat(out, ".%s(%s)", parameter.port_name,
                          parameter.expression->Emit(line_info));
    LineInfoIncrease(line_info, 1);
  };
  if (!parameters_.empty()) {
    absl::StrAppend(&result, "#(\n  ");
    LineInfoIncrease(line_info, 1);
    absl::StrAppend(&result,
                    absl::StrJoin(parameters_, ",\n  ", append_connection),
                    "\n) ");
  }
  absl::StrAppend(&result, instance_name_, " (\n  ");
  LineInfoIncrease(line_info, NumberOfNewlines(instance_name_) + 1);
  absl::StrAppend(
      &result, absl::StrJoin(connections_, ",\n  ", append_connection), "\n)");
  absl::StrAppend(&result, ";");
  LineInfoEnd(line_info, this);
  return result;
}

}  // namespace verilog
}  // namespace xls
