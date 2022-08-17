/*******************************************************************************
 * Copyright 2022 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#include <utility>
#include <vector>
#include "../viewer.hpp"
#include "../visitor.hpp"
#include "tensor2var.hpp"
#include <compiler/ir/builder.hpp>
#include <util/any_map.hpp>

namespace sc {

struct tensor2var_result_t {
    uint32_t simd_len_ = 0;
    size_t tensor_len_;
    bool can_replace_;
    bool defined_in_parallel_;
    std::vector<expr> to_replace_;
    std::vector<bool> referenced_;
    tensor2var_result_t(
            size_t tensor_len, bool can_replace, bool defined_in_parallel)
        : tensor_len_(tensor_len)
        , can_replace_(can_replace)
        , defined_in_parallel_(defined_in_parallel) {}
};

static tensor2var_result_t *get_result(const expr_c &v) {
    if (v->temp_data_) return v->temp_data_->get_or_null<tensor2var_result_t>();
    return nullptr;
}

class tensor2var_analysis_t : public ir_viewer_t {
public:
    using ir_viewer_t::dispatch;
    using ir_viewer_t::view;
    bool is_parallel_ = false;

    std::vector<sc_expr_type> expr_stack_;

    expr_c dispatch(expr_c v) override {
        expr_stack_.push_back(v->node_type_);
        auto ret = ir_viewer_t::dispatch(std::move(v));
        expr_stack_.pop_back();
        return ret;
    }

    void view(define_c v) override {
        if (v->linkage_ == linkage::local && v->var_.isa<tensor>()) {
            auto tsr = v->var_.static_as<tensor>();
            assert(tsr->dims_.size() == 1UL);
            size_t sz = 0;
            if (tsr->dims_.front().isa<constant>()) {
                sz = get_const_as_int(tsr->dims_.front().static_as<constant>());
            }
            bool no_tensor2var = v->var_->attr_
                    && v->var_->attr_->get_or_else(
                            attr_keys::no_tensor2var, false);
            v->var_->temp_data() = tensor2var_result_t {sz,
                    /*can_replace*/ sz != 0 && !no_tensor2var, is_parallel_};
        }
        if (v->init_.defined()) { dispatch(v->init_); }
    }

    void view(for_loop_c v) override {
        bool old_is_paralllel = is_parallel_;
        is_parallel_ |= v->kind_ == for_type::PARALLEL;
        ir_viewer_t::view(v);
        is_parallel_ = old_is_paralllel;
    }

    void view(tensor_c v) override {
        auto result = get_result(v);
        if (!result) { return; }
        if (expr_stack_.size() <= 1UL
                || expr_stack_[expr_stack_.size() - 2]
                        != sc_expr_type::indexing) {
            // if the tensor node is directly used without indexing node, like
            // `var b: pointer = A`, we cannot replace it
            result->can_replace_ = false;
            return;
        }
    }

    void view(tensorptr_c v) override {
        if (auto result = get_result(v->base_->ptr_)) {
            // if we take address of tensor, don't replace it with var
            result->can_replace_ = false;
        }
        ir_viewer_t::view(v);
    }

    void view(indexing_c v) override {
        if (auto result = get_result(v->ptr_)) {
            if (result->can_replace_) {
                if (result->defined_in_parallel_ != is_parallel_
                        || v->mask_.defined()) {
                    // if the tensor is defined out of parallel-for and we visit
                    // it in a parallel-for
                    result->can_replace_ = false;
                } else {
                    auto cur_simd = v->dtype_.lanes_;
                    if (result->simd_len_ == 0UL) {
                        result->simd_len_ = cur_simd;
                        if (result->tensor_len_ % result->simd_len_ != 0) {
                            result->can_replace_ = false;
                        }
                        if (result->tensor_len_ / result->simd_len_ > 8) {
                            result->can_replace_ = false;
                        }
                        result->referenced_ = std::vector<bool>(
                                result->tensor_len_ / result->simd_len_);
                    }
                    if (cur_simd != result->simd_len_) {
                        // if the tensor is accessed with different SIMD len
                        result->can_replace_ = false;
                    } else {
                        assert(v->idx_.size() == 1UL);
                        auto idx = v->idx_.front();
                        if (!idx.isa<constant>()) {
                            // if the indexing on the tensor is not constant
                            result->can_replace_ = false;
                        } else {
                            auto cidx = get_const_as_int(
                                    idx.static_as<constant>());
                            if (cidx % cur_simd != 0) {
                                // if the constant index is not compatible with
                                // SIMD length
                                result->can_replace_ = false;
                            }
                            result->referenced_.at(cidx / cur_simd) = true;
                        }
                    }
                }
            }
        }

        ir_viewer_t::view(v);
    }
}; // namespace sc

class tensor2var_replacer_t : public ir_visitor_t {
public:
    using ir_visitor_t::dispatch;
    using ir_visitor_t::visit;

    std::vector<stmt_c> *seq_ = nullptr;
    stmt_c visit(define_c v) override {
        if (auto result = get_result(v->var_)) {
            bool must_tensor2var = v->var_->attr_
                    && v->var_->attr_->get_or_else(
                            attr_keys::must_tensor2var, false);
            if (must_tensor2var) {
                COMPILE_ASSERT(result->can_replace_,
                        "The tensor " << v
                                      << " is marked must_tensor2var but "
                                         "cannot be replaced.");
            }
            if (result->can_replace_) {
                auto num_vars = result->simd_len_ == 0
                        ? 0
                        : result->tensor_len_ / result->simd_len_;

                auto tsr = v->var_.checked_as<tensor>();
                auto dtype = tsr->elem_dtype_;
                dtype.lanes_ = result->simd_len_;
                for (size_t i = 0; i < num_vars; i++) {
                    if (result->referenced_[i]) {
                        result->to_replace_.emplace_back(builder::make_var(
                                dtype, tsr->name_ + std::to_string(i)));
                        seq_->emplace_back(
                                builder::make_var_tensor_def_unattached(
                                        result->to_replace_[i]));
                    } else {
                        result->to_replace_.emplace_back(expr());
                    }
                }
                return stmt();
            }
        }
        return ir_visitor_t::visit(v);
    }

    expr_c visit(tensor_c v) override {
        auto result = get_result(v);
        assert(!result || !result->can_replace_);
        return ir_visitor_t::visit(v);
    }

    expr_c visit(indexing_c v) override {
        if (auto result = get_result(v->ptr_)) {
            if (result->can_replace_) {
                assert(v->idx_.size() == 1UL);
                auto idx = v->idx_.front();
                auto cidx = get_const_as_int(idx.checked_as<constant>());
                return result->to_replace_.at(cidx / result->simd_len_);
            }
        }

        return ir_visitor_t::visit(v);
    }

    stmt_c visit(stmts_c s) override {
        auto old_seq = seq_;
        std::vector<stmt_c> myseq;
        seq_ = &myseq;
        bool changed = false;
        for (auto &st : s->seq_) {
            auto ret = dispatch(st);
            changed |= !ret.ptr_same(st);
            if (ret.defined()) { myseq.emplace_back(ret); }
        }
        seq_ = old_seq;
        if (!changed) { return s; }
        return copy_attr(*s, builder::make_stmts_unattached(myseq));
    }
};

func_c tensor2var_t::operator()(func_c f) {
    tensor2var_analysis_t analy;
    analy.dispatch(f);
    tensor2var_replacer_t impl;
    return impl.dispatch(f);
}

} // namespace sc
