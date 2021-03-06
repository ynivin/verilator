// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Break always into sensitivity block domains
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2020 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// V3Scope's Transformations:
//
//      For every CELL that references this module, create a
//              SCOPE
//                      {all blocked statements}
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3Scope.h"
#include "V3Ast.h"

#include <algorithm>
#include <cstdarg>
#include <iomanip>
#include <map>
#include VL_INCLUDE_UNORDERED_MAP
#include VL_INCLUDE_UNORDERED_SET

//######################################################################
// Scope class functions

class ScopeVisitor : public AstNVisitor {
private:
    // NODE STATE
    // AstVar::user1p           -> AstVarScope replacement for this variable
    // AstTask::user2p          -> AstTask*.  Replacement task
    AstUser1InUse m_inuser1;
    AstUser2InUse m_inuser2;

    // TYPES
    typedef vl_unordered_map<AstNodeModule*, AstScope*> PackageScopeMap;
    // These cannot be unordered unless make a specialized hashing pair (gcc-8)
    typedef std::map<std::pair<AstVar*, AstScope*>, AstVarScope*> VarScopeMap;
    typedef std::set<std::pair<AstVarRef*, AstScope*> > VarRefScopeSet;

    // STATE, inside processing a single module
    AstNodeModule* m_modp;  // Current module
    AstScope* m_scopep;  // Current scope we are building
    // STATE, for passing down one level of hierarchy (may need save/restore)
    AstCell* m_aboveCellp;  // Cell that instantiates this module
    AstScope* m_aboveScopep;  // Scope that instantiates this scope

    PackageScopeMap m_packageScopes;  // Scopes for each package
    VarScopeMap m_varScopes;  // Varscopes created for each scope and var
    VarRefScopeSet m_varRefScopes;  // Varrefs-in-scopes needing fixup when done

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    void cleanupVarRefs() {
        for (VarRefScopeSet::iterator it = m_varRefScopes.begin(); it != m_varRefScopes.end();
             ++it) {
            AstVarRef* nodep = it->first;
            AstScope* scopep = it->second;
            if (nodep->packagep() && !nodep->varp()->isClassMember()) {
                PackageScopeMap::iterator it2 = m_packageScopes.find(nodep->packagep());
                UASSERT_OBJ(it2 != m_packageScopes.end(), nodep, "Can't locate package scope");
                scopep = it2->second;
            }
            VarScopeMap::iterator it3 = m_varScopes.find(make_pair(nodep->varp(), scopep));
            UASSERT_OBJ(it3 != m_varScopes.end(), nodep, "Can't locate varref scope");
            AstVarScope* varscp = it3->second;
            nodep->varScopep(varscp);
        }
    }

    // VISITORS
    virtual void visit(AstNetlist* nodep) VL_OVERRIDE {
        AstNodeModule* modp = nodep->topModulep();
        if (!modp) {
            nodep->v3error("No top level module found");
            return;
        }
        // Operate starting at the top of the hierarchy
        m_aboveCellp = NULL;
        m_aboveScopep = NULL;
        iterate(modp);
        cleanupVarRefs();
    }
    virtual void visit(AstNodeModule* nodep) VL_OVERRIDE {
        // Create required blocks and add to module
        string scopename;
        if (!m_aboveScopep) {
            scopename = "TOP";
        } else {
            scopename = m_aboveScopep->name() + "." + m_aboveCellp->name();
        }

        UINFO(4, " MOD AT " << scopename << "  " << nodep << endl);
        AstNode::user1ClearTree();

        m_scopep = new AstScope(
            (m_aboveCellp ? static_cast<AstNode*>(m_aboveCellp) : static_cast<AstNode*>(nodep))
                ->fileline(),
            nodep, scopename, m_aboveScopep, m_aboveCellp);
        if (VN_IS(nodep, Package)) {
            m_packageScopes.insert(make_pair(VN_CAST(nodep, Package), m_scopep));
        }

        // Now for each child cell, iterate the module this cell points to
        for (AstNode* cellnextp = nodep->stmtsp(); cellnextp; cellnextp = cellnextp->nextp()) {
            if (AstCell* cellp = VN_CAST(cellnextp, Cell)) {
                AstScope* oldScopep = m_scopep;
                AstCell* oldAbCellp = m_aboveCellp;
                AstScope* oldAbScopep = m_aboveScopep;
                {
                    m_aboveCellp = cellp;
                    m_aboveScopep = m_scopep;
                    AstNodeModule* modp = cellp->modp();
                    UASSERT_OBJ(modp, cellp, "Unlinked mod");
                    iterate(modp);  // Recursive call to visit(AstNodeModule)
                }
                // Done, restore vars
                m_scopep = oldScopep;
                m_aboveCellp = oldAbCellp;
                m_aboveScopep = oldAbScopep;
            }
        }

        // Create scope for the current usage of this module
        UINFO(4, " back AT " << scopename << "  " << nodep << endl);
        AstNode::user1ClearTree();
        m_modp = nodep;
        if (m_modp->isTop()) {
            AstTopScope* topscp = new AstTopScope(nodep->fileline(), m_scopep);
            m_modp->addStmtp(topscp);
        } else {
            m_modp->addStmtp(m_scopep);
        }

        // Copy blocks into this scope
        // If this is the first usage of the block ever, we can simply move the reference
        iterateChildren(nodep);

        // ***Note m_scopep is passed back to the caller of the routine (above)
    }
    virtual void visit(AstClass* nodep) VL_OVERRIDE {
        // Create required blocks and add to module
        AstScope* oldScopep = m_scopep;
        AstCell* oldAbCellp = m_aboveCellp;
        AstScope* oldAbScopep = m_aboveScopep;
        {
            m_aboveScopep = m_scopep;

            string scopename;
            if (!m_aboveScopep) {
                scopename = "TOP";
            } else {
                scopename = m_aboveScopep->name() + "." + nodep->name();
            }

            UINFO(4, " CLASS AT " << scopename << "  " << nodep << endl);
            AstNode::user1ClearTree();

            AstNode* abovep = (m_aboveCellp ? static_cast<AstNode*>(m_aboveCellp)
                                            : static_cast<AstNode*>(nodep));
            m_scopep
                = new AstScope(abovep->fileline(), m_modp, scopename, m_aboveScopep, m_aboveCellp);
            // Create scope for the current usage of this cell
            AstNode::user1ClearTree();
            nodep->addMembersp(m_scopep);

            iterateChildren(nodep);
        }
        // Done, restore vars
        m_scopep = oldScopep;
        m_aboveCellp = oldAbCellp;
        m_aboveScopep = oldAbScopep;
    }
    virtual void visit(AstCellInline* nodep) VL_OVERRIDE {  //
        nodep->scopep(m_scopep);
    }
    virtual void visit(AstActive* nodep) VL_OVERRIDE {
        nodep->v3fatalSrc("Actives now made after scoping");
    }
    virtual void visit(AstNodeProcedure* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    Move " << nodep << endl);
        AstNode* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        iterateChildren(clonep);  // We iterate under the *clone*
    }
    virtual void visit(AstAssignAlias* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    Move " << nodep << endl);
        AstNode* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        iterateChildren(clonep);  // We iterate under the *clone*
    }
    virtual void visit(AstAssignVarScope* nodep) VL_OVERRIDE {
        // Copy under the scope but don't recurse
        UINFO(4, "    Move " << nodep << endl);
        AstNode* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        iterateChildren(clonep);  // We iterate under the *clone*
    }
    virtual void visit(AstAssignW* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    Move " << nodep << endl);
        AstNode* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        iterateChildren(clonep);  // We iterate under the *clone*
    }
    virtual void visit(AstAlwaysPublic* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    Move " << nodep << endl);
        AstNode* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        iterateChildren(clonep);  // We iterate under the *clone*
    }
    virtual void visit(AstCoverToggle* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    Move " << nodep << endl);
        AstNode* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        iterateChildren(clonep);  // We iterate under the *clone*
    }
    virtual void visit(AstCFunc* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    CFUNC " << nodep << endl);
        AstCFunc* clonep = nodep->cloneTree(false);
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        clonep->scopep(m_scopep);
        // We iterate under the *clone*
        iterateChildren(clonep);
    }
    virtual void visit(AstNodeFTask* nodep) VL_OVERRIDE {
        // Add to list of blocks under this scope
        UINFO(4, "    FTASK " << nodep << endl);
        AstNodeFTask* clonep;
        if (nodep->classMethod()) {
            // Only one scope will be created, so avoid pointless cloning
            nodep->unlinkFrBack();
            clonep = nodep;
        } else {
            clonep = nodep->cloneTree(false);
        }
        nodep->user2p(clonep);
        m_scopep->addActivep(clonep);
        // We iterate under the *clone*
        iterateChildren(clonep);
    }
    virtual void visit(AstVar* nodep) VL_OVERRIDE {
        // Make new scope variable
        if (!nodep->user1p()) {
            AstVarScope* varscp = new AstVarScope(nodep->fileline(), m_scopep, nodep);
            UINFO(6, "   New scope " << varscp << endl);
            if (m_aboveCellp && !m_aboveCellp->isTrace()) varscp->trace(false);
            nodep->user1p(varscp);
            if (v3Global.opt.isClocker(varscp->prettyName())) {
                nodep->attrClocker(VVarAttrClocker::CLOCKER_YES);
            }
            if (v3Global.opt.isNoClocker(varscp->prettyName())) {
                nodep->attrClocker(VVarAttrClocker::CLOCKER_NO);
            }
            UASSERT_OBJ(m_scopep, nodep, "No scope for var");
            m_varScopes.insert(make_pair(make_pair(nodep, m_scopep), varscp));
            m_scopep->addVarp(varscp);
        }
    }
    virtual void visit(AstVarRef* nodep) VL_OVERRIDE {
        // VarRef needs to point to VarScope
        // Make sure variable has made user1p.
        UASSERT_OBJ(nodep->varp(), nodep, "Unlinked");
        if (nodep->varp()->isIfaceRef()) {
            nodep->varScopep(NULL);
        } else {
            // We may have not made the variable yet, and we can't make it now as
            // the var's referenced package etc might not be created yet.
            // So push to a list and post-correct
            m_varRefScopes.insert(make_pair(nodep, m_scopep));
        }
    }
    virtual void visit(AstScopeName* nodep) VL_OVERRIDE {
        // If there's a %m in the display text, we add a special node that will contain the name()
        string prefix = string("__DOT__") + m_scopep->name();
        // TOP and above will be the user's name().
        // Note 'TOP.' is stripped by scopePrettyName
        // To keep correct visual order, must add before other Text's
        AstNode* afterp = nodep->scopeAttrp();
        if (afterp) afterp->unlinkFrBackWithNext();
        nodep->scopeAttrp(new AstText(nodep->fileline(), prefix));
        if (afterp) nodep->scopeAttrp(afterp);
        afterp = nodep->scopeEntrp();
        if (afterp) afterp->unlinkFrBackWithNext();
        nodep->scopeEntrp(new AstText(nodep->fileline(), prefix));
        if (afterp) nodep->scopeEntrp(afterp);
        iterateChildren(nodep);
    }
    virtual void visit(AstScope* nodep) VL_OVERRIDE {
        // Scope that was made by this module for different cell;
        // Want to ignore blocks under it, so just do nothing
    }
    //--------------------
    virtual void visit(AstNode* nodep) VL_OVERRIDE { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit ScopeVisitor(AstNetlist* nodep) {
        m_aboveCellp = NULL;
        m_aboveScopep = NULL;
        m_modp = NULL;
        m_scopep = NULL;
        //
        iterate(nodep);
    }
    virtual ~ScopeVisitor() {}
};

//######################################################################
// Scope cleanup -- remove unused activates

class ScopeCleanupVisitor : public AstNVisitor {
private:
    // STATE
    AstScope* m_scopep;  // Current scope we are building

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstScope* nodep) VL_OVERRIDE {
        // Want to ignore blocks under it
        m_scopep = nodep;
        iterateChildren(nodep);
        m_scopep = NULL;
    }

    virtual void movedDeleteOrIterate(AstNode* nodep) {
        if (m_scopep) {
            // The new block; repair varrefs
            iterateChildren(nodep);
        } else {
            // A block that was just moved under a scope, Kill it.
            // Certain nodes can be referenced later in this pass, notably
            // an FTaskRef needs to access the FTask to find the cloned task
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
        }
    }

    virtual void visit(AstNodeProcedure* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstAssignAlias* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstAssignVarScope* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstAssignW* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstAlwaysPublic* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstCoverToggle* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstNodeFTask* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }
    virtual void visit(AstCFunc* nodep) VL_OVERRIDE { movedDeleteOrIterate(nodep); }

    virtual void visit(AstVarXRef* nodep) VL_OVERRIDE {
        // The crossrefs are dealt with in V3LinkDot
        nodep->varp(NULL);
    }
    virtual void visit(AstNodeFTaskRef* nodep) VL_OVERRIDE {
        // The crossrefs are dealt with in V3LinkDot
        UINFO(9, "   Old pkg-taskref " << nodep << endl);
        if (nodep->packagep()) {
            // Point to the clone
            UASSERT_OBJ(nodep->taskp(), nodep, "Unlinked");
            AstNodeFTask* newp = VN_CAST(nodep->taskp()->user2p(), NodeFTask);
            UASSERT_OBJ(newp, nodep, "No clone for package function");
            nodep->taskp(newp);
            UINFO(9, "   New pkg-taskref " << nodep << endl);
        } else if (!VN_IS(nodep, MethodCall)) {
            nodep->taskp(NULL);
            UINFO(9, "   New pkg-taskref " << nodep << endl);
        }
        iterateChildren(nodep);
    }
    virtual void visit(AstModportFTaskRef* nodep) VL_OVERRIDE {
        // The crossrefs are dealt with in V3LinkDot
        nodep->ftaskp(NULL);
        iterateChildren(nodep);
    }

    //--------------------
    virtual void visit(AstNode* nodep) VL_OVERRIDE { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit ScopeCleanupVisitor(AstNetlist* nodep) {
        m_scopep = NULL;
        iterate(nodep);
    }
    virtual ~ScopeCleanupVisitor() {}
};

//######################################################################
// Scope class functions

void V3Scope::scopeAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    {
        ScopeVisitor visitor(nodep);
        ScopeCleanupVisitor cleanVisitor(nodep);
    }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("scope", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
