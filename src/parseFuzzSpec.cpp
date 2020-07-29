#include "parseFuzzSpec.hpp"

std::set<fuzzVarDecl, decltype(&fuzzVarDecl::compare)> declared_fuzz_vars;

static std::map<std::string, clang::APValue*> config_inputs;
static std::pair<const clang::CallExpr*, const clang::CallExpr*>
    fuzz_template_bounds(nullptr, nullptr);
static std::set<clang::VarDecl*> input_template_var_decls;
static std::map<size_t, std::vector<std::string>>
    input_template_copies;
static const clang::CompoundStmt* main_child;

static std::vector<fuzzNewCall> fuzz_new_vars;
static std::vector<std::pair<const clang::CallExpr*, const clang::Stmt*>> mr_fuzz_calls;
static std::vector<stmtRedeclTemplateVars> stmt_rewrite_map;
static std::set<fuzzVarDecl, decltype(&fuzzVarDecl::compare)>
    common_template_var_decls(&fuzzVarDecl::compare);

bool
inFuzzTemplate(const clang::Decl* d, clang::SourceManager& SM)
{
    if (!fuzz_template_bounds.first || !fuzz_template_bounds.second)
    {
        return false;
    }
    clang::BeforeThanCompare<clang::SourceLocation> btc(SM);
    return btc(d->getBeginLoc(), fuzz_template_bounds.second->getEndLoc()) &&
        btc(fuzz_template_bounds.first->getBeginLoc(), d->getEndLoc());
}

void
fuzzConfigRecorder::run(const clang::ast_matchers::MatchFinder::MatchResult& Result)
{
    if (const clang::VarDecl* VD =
            Result.Nodes.getNodeAs<clang::VarDecl>("inputDecl"))
    {
        config_inputs.insert(std::make_pair(VD->getNameAsString(), VD->evaluateValue()));
    }
}

fuzzConfigParser::fuzzConfigParser()
{
    matcher.addMatcher(
        clang::ast_matchers::varDecl(
        clang::ast_matchers::hasAncestor(
        clang::ast_matchers::namespaceDecl(
        clang::ast_matchers::hasName(
        "fuzz::input"))))
            .bind("inputDecl"), &recorder);
}

void
fuzzConfigParser::HandleTranslationUnit(clang::ASTContext& ctx)
{
    matcher.matchAST(ctx);
}

std::string
templateVariableDuplicatorVisitor::getText()
{
    std::string ss_str;
    llvm::raw_string_ostream ss(ss_str);
    rw.getEditBuffer(rw.getSourceMgr().getMainFileID()).write(ss);
    return ss.str();
}

bool
templateVariableDuplicatorVisitor::VisitVarDecl(clang::VarDecl* vd)
{
    rw.InsertText(vd->getLocation().getLocWithOffset(vd->getName().size()),
        "_" + std::to_string(id));
    return true;
}

bool
templateVariableDuplicatorVisitor::VisitDeclRefExpr(clang::DeclRefExpr* dre)
{
    if (llvm::dyn_cast<clang::VarDecl>(dre->getDecl()))
    {
        rw.InsertText(dre->getEndLoc(), "_" + std::to_string(id));
    }
    return true;
}

//==============================================================================

const clang::ast_type_traits::DynTypedNode
parseFuzzConstructsVisitor::getBaseParent(
    const clang::ast_type_traits::DynTypedNode dyn_node)
{
    clang::ASTContext::DynTypedNodeList node_parents =
        this->ctx.getParents(dyn_node);
    assert(node_parents.size() == 1);
    if (node_parents[0].get<clang::CompoundStmt>() == main_child)
    {
        return dyn_node;
    }
    return this->getBaseParent(node_parents[0]);
}

bool
parseFuzzConstructsVisitor::VisitCallExpr(clang::CallExpr* ce)
{
    if (clang::Decl* d = ce->getCalleeDecl())
    {
        if (clang::FunctionDecl* fd =
                llvm::dyn_cast<clang::FunctionDecl>(d);
            fd && !fd->getQualifiedNameAsString().compare("fuzz::start"))
        {
            assert(!in_fuzz_template);
            in_fuzz_template = true;
        }
        else if (clang::FunctionDecl* fd =
                llvm::dyn_cast<clang::FunctionDecl>(d);
            fd && !fd->getQualifiedNameAsString().compare("fuzz::end"))
        {
            assert(in_fuzz_template);
            in_fuzz_template = false;
        }
    }
    return true;
}

bool
parseFuzzConstructsVisitor::VisitVarDecl(clang::VarDecl* vd)
{
    if (in_fuzz_template)
    {
        //std::cout << vd->getNameAsString() << std::endl;
        //common_template_var_decls.erase(vd);
        clang::ASTContext::DynTypedNodeList vd_parents =
            this->ctx.getParents(*vd);
        assert(vd_parents.size() == 1);
        const clang::Stmt* base_parent =
            this->getBaseParent(vd_parents[0]).get<clang::Stmt>();
        assert(base_parent);
        std::vector<stmtRedeclTemplateVars>::iterator srtv_it =
            std::find_if(stmt_rewrite_map.begin(), stmt_rewrite_map.end(),
            [&base_parent](stmtRedeclTemplateVars srtv)
            {
                return srtv.base_stmt == base_parent;
            });
        assert(srtv_it != stmt_rewrite_map.end());
        (*srtv_it).decl_var_additions.push_back(
            vd->getLocation().getLocWithOffset(vd->getName().size()));
        input_template_var_decls.insert(vd);
        declared_fuzz_vars.emplace(vd->getNameAsString(),
            vd->getType().getAsString());
    }
    return true;
}

bool
parseFuzzConstructsVisitor::VisitDeclRefExpr(clang::DeclRefExpr* dre)
{
    if (clang::VarDecl* vd = llvm::dyn_cast<clang::VarDecl>(dre->getDecl());
        vd && input_template_var_decls.count(vd))
    {
        clang::ASTContext::DynTypedNodeList dre_parents =
            this->ctx.getParents(*dre);
        assert(dre_parents.size() == 1);
        const clang::Stmt* base_parent =
            this->getBaseParent(dre_parents[0]).get<clang::Stmt>();
        assert(base_parent);
        std::vector<stmtRedeclTemplateVars>::iterator srtv_it =
            std::find_if(stmt_rewrite_map.begin(), stmt_rewrite_map.end(),
            [&base_parent](stmtRedeclTemplateVars srtv)
            {
                return srtv.base_stmt == base_parent;
            });
        assert(srtv_it != stmt_rewrite_map.end());
        (*srtv_it).decl_var_additions.push_back(
            dre->getLocation().getLocWithOffset(
                dre->getDecl()->getName().size()));

        //stmt_rewrite_map.at(base_parent).push_back(
            //dre->getLocation().getLocWithOffset(
                //dre->getDecl()->getName().size()));
    }
    if (dre->hasQualifier())
    {
        clang::NamespaceDecl* nd = dre->getQualifier()->getAsNamespace();
        if (nd && !nd->getNameAsString().compare("fuzz"))
        {
            if (!dre->getDecl()->getNameAsString().compare(meta_input_var_prefix))
            {
                clang::ASTContext::DynTypedNodeList dre_parents =
                    this->ctx.getParents(*dre);
                assert(dre_parents.size() == 1);
                const clang::Stmt* base_parent =
                    this->getBaseParent(dre_parents[0]).get<clang::Stmt>();
                assert(base_parent);
                std::vector<stmtRedeclTemplateVars>::iterator srtv_it =
                    std::find_if(stmt_rewrite_map.begin(), stmt_rewrite_map.end(),
                    [&base_parent](stmtRedeclTemplateVars srtv)
                    {
                        return srtv.base_stmt == base_parent;
                    });
                assert(srtv_it != stmt_rewrite_map.end());

                if (this->first_output_var)
                {
                    //to_replace << dre->getType().getAsString() << " ";
                    (*srtv_it).output_var_type = dre->getType().getAsString();
                    (*srtv_it).output_var_decl = dre->getSourceRange();
                    this->first_output_var = false;

                    if (meta_input_var_type.empty())
                    {
                        meta_input_var_type = dre->getType().getAsString();
                    }
                    assert(!dre->getType().getAsString().compare(meta_input_var_type));
                }
                else
                {
                    (*srtv_it).output_var_additions.push_back(dre->getSourceRange());
                }
                //for (size_t i = 0; i < meta_input_fuzz_count; ++i)
                //{
                    //std::stringstream to_replace_tmp(to_replace.str());
                    //clang::Rewriter rw_tmp(rw.getSourceMgr(), rw.getLangOpts());
                    //rw_tmp.ReplaceText(sr, llvm::StringRef(to_replace_tmp.str()));
                    //input_template_copies.at(i).push_back(
                        //rw_tmp.getRewrittenText(sr));
                //}
                //rw.RemoveText(sr);
            }
        }
    }
    return true;
}

//==============================================================================

std::set<std::pair<std::string, std::string>>
fuzzExpander::getDuplicateDeclVars(
    std::set<fuzzVarDecl, decltype(&fuzzVarDecl::compare)> vars,
    size_t output_var_count)
{
    std::set<std::pair<std::string, std::string>> duplicate_vars;
    for (fuzzVarDecl fvd : vars)
    {
        duplicate_vars.emplace(
            fvd.name + "_" + std::to_string(output_var_count),
            fvd.type);
    }
    std::for_each(common_template_var_decls.begin(),
        common_template_var_decls.end(),
        [&duplicate_vars](fuzzVarDecl fvd)
        {
            duplicate_vars.emplace(fvd.name, fvd.type);
        });

    //for (const clang::VarDecl* vd : common_template_var_decls)
    //{
        //vd->dump();
        //std::cout << vd->getNameAsString() << std::endl;
        //duplicate_vars.emplace(vd->getNameAsString(),
            //vd->getType().getAsString());
    //}
    return duplicate_vars;
}

void
fuzzExpander::expandLoggedNewVars(clang::Rewriter& rw, clang::ASTContext& ctx)
{
    size_t curr_input_count = 0;
    fuzzer::clang::resetApiObjs(
        getDuplicateDeclVars(declared_fuzz_vars, curr_input_count));
    for (fuzzNewCall fnc : fuzz_new_vars)
    {
        if (fnc.start_fuzz_call)
        {
            assert(false);
            assert(!fnc.base_stmt && !fnc.fuzz_ref &&
                !fnc.reset_fuzz_var_decl);
            rw.RemoveText(fnc.start_fuzz_call->getSourceRange());
            continue;
        }
        if (fnc.reset_fuzz_var_decl)
        {
            assert(!fnc.base_stmt && !fnc.fuzz_ref &&
                !fnc.start_fuzz_call && fnc.reset_fuzz_call);
            fuzzer::clang::resetApiObjs(
                getDuplicateDeclVars(
                    declared_fuzz_vars, ++curr_input_count));
            rw.RemoveText(fnc.reset_fuzz_call->getSourceRange());
            continue;
        }
        const clang::Stmt* base_stmt = fnc.base_stmt;
        const clang::DeclRefExpr* fuzz_ref = fnc.fuzz_ref;
        const clang::Stmt* fuzz_call = fuzz_ref;
        const llvm::StringRef indent =
            clang::Lexer::getIndentationForLine(base_stmt->getBeginLoc(),
                rw.getSourceMgr());
        while (true)
        {
            auto it = ctx.getParents(*fuzz_call).begin();
            fuzz_call = it->get<clang::Expr>();
            if (llvm::dyn_cast<clang::CallExpr>(fuzz_call))
            {
                break;
            }
        }
        assert(fuzz_ref->getNumTemplateArgs() == 1);
        std::pair<std::string, std::string> fuzzer_output =
            fuzzer::clang::generateObjectInstructions(
                fuzz_ref->template_arguments()[0].getArgument()
                .getAsType().getAsString(), indent);
        rw.InsertText(base_stmt->getBeginLoc(), fuzzer_output.first + indent.str());
        rw.ReplaceText(clang::SourceRange(fuzz_call->getBeginLoc(),
            fuzz_call->getEndLoc()), fuzzer_output.second);
    }
}

void fuzzExpander::expandLoggedNewMRVars(clang::Rewriter& rw, clang::ASTContext& ctx)
{
    std::set<std::pair<std::string, std::string>> mr_vars;

    // Add MR parameter vars
    fuzzer::clang::resetApiObjs(mr_vars);
    for (std::pair<const clang::CallExpr*, const clang::Stmt*> mrfc : mr_fuzz_calls)
    {
        const clang::CallExpr* ce = mrfc.first;
        const clang::Stmt* base_stmt = mrfc.second;

        const llvm::StringRef indent =
            clang::Lexer::getIndentationForLine(base_stmt->getBeginLoc(),
                rw.getSourceMgr());
        const clang::DeclRefExpr* ce_dre =
            llvm::dyn_cast<clang::DeclRefExpr>(
                llvm::dyn_cast<clang::ImplicitCastExpr>(ce->getCallee())->getSubExpr());
        assert(ce_dre);
        std::pair<std::string, std::string> fuzzer_output =
            fuzzer::clang::generateObjectInstructions(
                ce_dre->template_arguments()[0].getArgument().getAsType().getAsString(), "\t");
        rw.InsertText(base_stmt->getBeginLoc(), fuzzer_output.first + indent.str());
        rw.ReplaceText(clang::SourceRange(ce->getBeginLoc(),
            ce->getEndLoc()), fuzzer_output.second);
    }

}

//==============================================================================

void
newVariableFuzzerParser::run(
    const clang::ast_matchers::MatchFinder::MatchResult& Result)
{
    fuzzNewCall fnc;
    if (const clang::Stmt* s =
            Result.Nodes.getNodeAs<clang::Stmt>("baseStmt"))
    {
        fnc.base_stmt = s;
    }
    if (const clang::DeclRefExpr* dre =
            Result.Nodes.getNodeAs<clang::DeclRefExpr>("fuzzRef"))
    {
        fnc.fuzz_ref = dre;
    }
    if (const clang::CallExpr* ce =
            Result.Nodes.getNodeAs<clang::CallExpr>("outputVarEnd"))
    {
        fnc.reset_fuzz_call = ce;
        fnc.reset_fuzz_var_decl = true;
    }
    fuzz_new_vars.push_back(fnc);
}

void
mrNewVariableFuzzerLogger::run(
    const clang::ast_matchers::MatchFinder::MatchResult& Result)
{
    const clang::CallExpr* ce = Result.Nodes.getNodeAs<clang::CallExpr>("fuzzRef");
    assert(ce);
    const clang::Stmt* base_stmt = Result.Nodes.getNodeAs<clang::Stmt>("baseStmt");
    assert(base_stmt);
    mr_fuzz_calls.push_back(std::make_pair(ce, base_stmt));
}

//==============================================================================

void
newVariableStatementRemover::run(
    const clang::ast_matchers::MatchFinder::MatchResult& Result)
{
    if (const clang::CallExpr* ce =
            Result.Nodes.getNodeAs<clang::CallExpr>("outputVarStart"))
    {
        rw.RemoveText(ce->getSourceRange());
    }
    else if (const clang::NullStmt* ns =
            Result.Nodes.getNodeAs<clang::NullStmt>("empty"))
    {
        rw.RemoveText(ns->getSourceRange());
    }
}

//==============================================================================

newVariableFuzzerMatcher::newVariableFuzzerMatcher(clang::Rewriter& _rw) :
    remover(newVariableStatementRemover(_rw))
{
            matcher.addMatcher(
                clang::ast_matchers::stmt(
                clang::ast_matchers::allOf(
                /* Base stmt two away from main.. */
                clang::ast_matchers::hasParent(
                clang::ast_matchers::stmt(
                clang::ast_matchers::hasParent(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::isMain())))),
                /* .. which contains call to fuzz_new */
                clang::ast_matchers::hasDescendant(
                clang::ast_matchers::declRefExpr(
                clang::ast_matchers::to(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::hasName(
                "fuzz::fuzz_new"))))
                    .bind("fuzzRef"))) )
                    .bind("baseStmt"), &parser);

            matcher.addMatcher(
                clang::ast_matchers::callExpr(
                clang::ast_matchers::allOf(
                clang::ast_matchers::unless(
                clang::ast_matchers::hasAncestor(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::isMain()))),

                clang::ast_matchers::hasAncestor(
                clang::ast_matchers::stmt(
                clang::ast_matchers::hasParent(
                clang::ast_matchers::compoundStmt(
                clang::ast_matchers::hasParent(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::hasParent(
                clang::ast_matchers::translationUnitDecl()))))))
                    .bind("baseStmt")),

                clang::ast_matchers::callee(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::hasName(
                "fuzz::fuzz_new")))))
                    .bind("fuzzRef"), &mr_fuzzer_logger);

            matcher.addMatcher(
                clang::ast_matchers::callExpr(
                clang::ast_matchers::callee(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::hasName(
                "fuzz::end"))))
                    .bind("outputVarEnd"), &parser);

            matcher.addMatcher(
                clang::ast_matchers::callExpr(
                clang::ast_matchers::callee(
                clang::ast_matchers::functionDecl(
                clang::ast_matchers::hasName(
                "fuzz::start"))))
                    .bind("outputVarStart"), &remover);
}

//==============================================================================

void
templateLocLogger::run(const clang::ast_matchers::MatchFinder::MatchResult& Result)
{
    if (const clang::CallExpr* start_ce =
            Result.Nodes.getNodeAs<clang::CallExpr>("startTemplate"))
    {
        fuzz_template_bounds.first = start_ce;
    }
    else if (const clang::CallExpr* end_ce =
            Result.Nodes.getNodeAs<clang::CallExpr>("endTemplate"))
    {
        fuzz_template_bounds.second = end_ce;
    }
    else if (const clang::CompoundStmt* cs =
            Result.Nodes.getNodeAs<clang::CompoundStmt>("mainChild"))
    {
        main_child = cs;
    }
    else if (const clang::VarDecl* vd =
            Result.Nodes.getNodeAs<clang::VarDecl>("mainVarDecl");
            vd && !inFuzzTemplate(vd, SM))
    {
        //vd->dump();
        common_template_var_decls.emplace(vd->getNameAsString(),
            vd->getType().getAsString());
    }
}

//==============================================================================

templateDuplicator::templateDuplicator(clang::Rewriter& _rw) :
    rw(_rw), logger(templateLocLogger(_rw.getSourceMgr()))
{
    matcher.addMatcher(
        clang::ast_matchers::callExpr(
        clang::ast_matchers::callee(
        clang::ast_matchers::functionDecl(
        clang::ast_matchers::hasName(
        "fuzz::end"))))
            .bind("endTemplate"), &logger);

    matcher.addMatcher(
        clang::ast_matchers::callExpr(
        clang::ast_matchers::callee(
        clang::ast_matchers::functionDecl(
        clang::ast_matchers::hasName(
        "fuzz::start"))))
            .bind("startTemplate"), &logger);

    matcher.addMatcher(
        clang::ast_matchers::compoundStmt(
        clang::ast_matchers::hasParent(
        clang::ast_matchers::functionDecl(
        clang::ast_matchers::isMain())))
            .bind("mainChild"), &logger);

    matcher.addMatcher(
        clang::ast_matchers::varDecl(
        clang::ast_matchers::allOf(
            clang::ast_matchers::hasAncestor(
            clang::ast_matchers::functionDecl(
            clang::ast_matchers::isMain()))
            ,
            clang::ast_matchers::unless(
            clang::ast_matchers::parmVarDecl())
        ))
            .bind("mainVarDecl"), &logger);

    for (size_t i = 0; i < meta_input_fuzz_count; ++i)
    {
        std::vector<std::string> input_template_strs;
        input_template_strs.push_back(
            "/* Template initialisation for output var " +
            std::to_string(i) + " */\n");
        std::pair<size_t, std::vector<std::string>> template_str_pair(i, input_template_strs);
        input_template_copies.insert(template_str_pair);
    }
};

void
templateDuplicator::HandleTranslationUnit(clang::ASTContext& ctx)
{
    matcher.matchAST(ctx);

    bool in_fuzz_template = false;
    for (clang::Stmt::const_child_iterator it = main_child->child_begin();
        it != main_child->child_end(); ++it)
    {
        if (in_fuzz_template)
        {
            stmt_rewrite_map.emplace_back(*it);
        }
        if (const clang::CallExpr* call_child =
            llvm::dyn_cast<clang::CallExpr>(*it))
        {
            if (const clang::FunctionDecl* fn_child =
                llvm::dyn_cast<clang::CallExpr>(*it)->getDirectCallee())
            {
                if (!fn_child->getQualifiedNameAsString()
                        .compare("fuzz::start"))
                {
                    assert(!in_fuzz_template);
                    in_fuzz_template = true;
                }
                else if (!fn_child->getQualifiedNameAsString()
                        .compare("fuzz::end"))
                {
                    assert(in_fuzz_template);
                    in_fuzz_template = false;
                }
            }
        }
    }

    parseFuzzConstructsVisitor parseConstructsVis(rw, ctx);
    parseConstructsVis.TraverseDecl(ctx.getTranslationUnitDecl());

    for (stmtRedeclTemplateVars stmt_redecl :
                stmt_rewrite_map)
    {
        //stmt_redecl.base_stmt->dump();
        const llvm::StringRef indent =
            clang::Lexer::getIndentationForLine(
                stmt_redecl.base_stmt->getBeginLoc(),
                rw.getSourceMgr());
        for (size_t i = 0; i < meta_input_fuzz_count; ++i)
        {
            clang::Rewriter rw_tmp(rw.getSourceMgr(), rw.getLangOpts());
            for (clang::SourceLocation sl :
                    stmt_redecl.decl_var_additions)
            {
                rw_tmp.InsertText(sl, "_" + std::to_string(i));
            }
            if (stmt_redecl.output_var_decl.isValid())
            {
                std::stringstream output_var_decl_rw;
                output_var_decl_rw << stmt_redecl.output_var_type;
                output_var_decl_rw << " " << meta_input_var_prefix << i;
                rw_tmp.ReplaceText(stmt_redecl.output_var_decl,
                    output_var_decl_rw.str());
            }
            for (clang::SourceRange sr :
                    stmt_redecl.output_var_additions)
            {
                rw_tmp.ReplaceText(sr, meta_input_var_prefix + std::to_string(i));
            }
            input_template_copies.at(i).
                push_back((indent + rw_tmp.getRewrittenText(
                    stmt_redecl.base_stmt->getSourceRange())).str());
        }
        rw.RemoveText(stmt_redecl.base_stmt->getSourceRange());
    }

    assert(fuzz_template_bounds.second);
    for (std::pair<size_t, std::vector<std::string>> fuzzed_template_copies :
            input_template_copies)
    {
        std::string template_copy_str = std::accumulate(
            fuzzed_template_copies.second.begin(),
            fuzzed_template_copies.second.end(), std::string(),
            [](std::string acc, std::string next_instr)
            {
                return acc + next_instr + ";\n";

            });
        rw.InsertText(fuzz_template_bounds.second->getBeginLoc(),
            template_copy_str);
    }

    //rw.RemoveText(fuzz_template_bounds.first->getSourceRange());

};

bool
templateDuplicatorAction::BeginSourceFileAction(clang::CompilerInstance& ci)
{
    std::cout << "[templateDuplicatorAction] Parsing input file ";
    std::cout << ci.getSourceManager().getFileEntryForID(
        ci.getSourceManager().getMainFileID())->getName().str()
        << std::endl;
    return true;
};

void
templateDuplicatorAction::EndSourceFileAction()
{
    //llvm::raw_string_ostream rso(rewrite_data);
    //rw.getEditBuffer(rw.getSourceMgr().getMainFileID()).write(rso);

    std::error_code ec;
    int fd;
    llvm::sys::fs::createTemporaryFile("", ".cpp", fd,
        rewritten_input_file);
    llvm::raw_fd_ostream rif_rfo(fd, true);
    rw.getEditBuffer(rw.getSourceMgr().getMainFileID()).write(rif_rfo);
    //
    //rw.getEditBuffer(rw.getSourceMgr().getMainFileID())
        //.write(llvm::outs());
}

bool
parseFuzzConstructsAction::BeginSourceFileAction(clang::CompilerInstance& ci)
{
    std::cout << "[parseFuzzConstructsAction] Parsing input file ";
    std::cout << ci.getSourceManager().getFileEntryForID(
        ci.getSourceManager().getMainFileID())->getName().str()
            << std::endl;
    return true;
}

void
parseFuzzConstructsAction::EndSourceFileAction()
{
    //llvm::raw_string_ostream rso(rewrite_data);
    //rw.getEditBuffer(rw.getSourceMgr().getMainFileID()).write(rso);

    std::error_code ec;
    int fd;
    llvm::sys::fs::createTemporaryFile("mtFuzz", "cpp", fd,
        rewritten_input_file);
    llvm::raw_fd_ostream rif_rfo(fd, true);
    rw.getEditBuffer(rw.getSourceMgr().getMainFileID()).write(rif_rfo);
    //llvm::sys::fs::remove(rewritten_input_file);

}
