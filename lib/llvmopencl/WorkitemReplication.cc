// LLVM function pass to replicate the kernel body for all work items 
// in a work group.
// 
// Copyright (c) 2011-2012 Carlos Sánchez de La Lama / URJC and
//                         Pekka Jääskeläinen / TUT
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define DEBUG_TYPE "workitem"

#include "WorkitemReplication.h"
#include "Workgroup.h"
#include "Barrier.h"
#include "Kernel.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ValueSymbolTable.h"
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

//#define DEBUG_BB_MERGING
//#define DUMP_RESULT_CFG
//#define DEBUG_PR_REPLICATION
//#define DEBUG_REFERENCE_FIXING

#ifdef DUMP_RESULT_CFG
#include "llvm/Analysis/CFGPrinter.h"
#endif

using namespace llvm;
using namespace pocl;

static bool block_has_barrier(const BasicBlock *bb);

STATISTIC(ContextValues, "Number of SSA values which have to be context-saved");
STATISTIC(ContextSize, "Context size per workitem in bytes");

namespace {
  static
  RegisterPass<WorkitemReplication> X("workitem", "Workitem replication pass");
}

cl::list<int>
LocalSize("local-size",
	  cl::desc("Local size (x y z)"),
	  cl::multi_val(3));
static cl::opt<bool>
AddWIMetadata("add-wi-metadata", cl::init(false), cl::Hidden,
  cl::desc("Adds a work item identifier to each of the instruction in work items."));

char WorkitemReplication::ID = 0;

void
WorkitemReplication::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<DominatorTree>();
  AU.addRequired<LoopInfo>();
  AU.addRequired<TargetData>();
}

bool
WorkitemReplication::dominatesUse(Instruction &I, unsigned i) {
  Instruction *Op = cast<Instruction>(I.getOperand(i));
  BasicBlock *OpBlock = Op->getParent();
  PHINode *PN = dyn_cast<PHINode>(&I);

  // DT can handle non phi instructions for us.
  if (!PN) 
    {
      // Definition must dominate use unless use is unreachable!
      return Op->getParent() == I.getParent() ||
        DT->dominates(Op, &I);
    }

  // PHI nodes are more difficult than other nodes because they actually
  // "use" the value in the predecessor basic blocks they correspond to.
  unsigned j = PHINode::getIncomingValueNumForOperand(i);
  BasicBlock *PredBB = PN->getIncomingBlock(j);
  return (PredBB && DT->dominates(OpBlock, PredBB));
}


bool
WorkitemReplication::runOnFunction(Function &F)
{
  if (!Workgroup::isKernelToProcess(F))
    return false;

  DT = &getAnalysis<DominatorTree>();
  LI = &getAnalysis<LoopInfo>();

  bool changed = ProcessFunction(F);
#ifdef DUMP_RESULT_CFG
  FunctionPass* cfgPrinter = createCFGPrinterPass();
  cfgPrinter->runOnFunction(F);
#endif

  changed |= fixUndominatedVariableUses(F);
  return changed;
}


/* Fixes the undominated variable uses.

   These appear when a conditional barrier kernel is replicated to
   form a copy of the *same basic block* in the alternative 
   "barrier path".

   E.g., from

   A -> [exit], A -> B -> [exit]

   a replicated CFG as follows, is created:

   A1 -> (T) A2 -> [exit1],  A1 -> (F) A2' -> B1, B2 -> [exit2] 

   The regions are correct because of the barrier semantics
   of "all or none". In case any barrier enters the [exit1]
   from A1, all must (because there's a barrier in the else
   branch).

   Here at A2 and A2' one creates the same variables. 
   However, B2 does not know which copy
   to refer to, the ones created in A2 or ones in A2' (correct).
   The mapping data contains only one possibility, the
   one that was placed there last. Thus, the instructions in B2 
   might end up referring to the variables defined in A2 
   which do not nominate them.

   The variable references are fixed by exploiting the knowledge
   of the naming convention of the cloned variables. 

   One potential alternative way would be to collect the refmaps per BB,
   not globally. Then as a final phase traverse through the 
   basic blocks starting from the beginning and propagating the
   reference data downwards, the data from the new BB overwriting
   the old one. This should ensure the reachability without 
   the costly dominance analysis.
*/
bool
WorkitemReplication::fixUndominatedVariableUses(llvm::Function &F) 
{
  bool changed = false;
  DT = &getAnalysis<DominatorTree>();
  DT->runOnFunction(F);
  LI->runOnFunction(F);

  for (Function::iterator i = F.begin(), e = F.end(); i != e; ++i) 
    {
      llvm::BasicBlock *bb = i;
      for (llvm::BasicBlock::iterator ins = bb->begin(), inse = bb->end();
           ins != inse; ++ins)
        {
          for (unsigned opr = 0; opr < ins->getNumOperands(); ++opr)
            {
              if (!isa<Instruction>(ins->getOperand(opr))) continue;
              Instruction *operand = cast<Instruction>(ins->getOperand(opr));
              if (dominatesUse(*ins, opr)) 
                  continue;
#ifdef DEBUG_REFERENCE_FIXING
              std::cout << "### dominance error!" << std::endl;
              operand->dump();
              std::cout << "### does not dominate:" << std::endl;
              ins->dump();
#endif
              StringRef baseName;
              std::pair< StringRef, StringRef > pieces = 
                operand->getName().rsplit('.');
              if (pieces.second.startswith("pocl_"))
                baseName = pieces.first;
              else
                baseName = operand->getName();
              
              Value *alternative = NULL;

              unsigned int copy_i = 0;
              do {
                std::ostringstream alternativeName;
                alternativeName << baseName.str();
                if (copy_i > 0)
                  alternativeName << ".pocl_" << copy_i;
#ifdef DEBUG_REFERENCE_FIXING
                std::cout << "### trying to find alternative variable:" 
                          << alternativeName.str() << std::endl;
#endif

                alternative = 
                  F.getValueSymbolTable().lookup(alternativeName.str());

#ifdef DEBUG_REFERENCE_FIXING
                if (alternative == NULL)
                  std::cout << "### did not find it!" << std::endl;
#endif
                if (alternative != NULL)
                  {
                    ins->setOperand(opr, alternative);
                    if (dominatesUse(*ins, opr))
                      break;
                  }
                     
                if (copy_i > UINT_MAX && alternative == NULL)
                  break; /* ran out of possibilities */
                ++copy_i;
              } while (true);

              if (alternative != NULL)
                {
#ifdef DEBUG_REFERENCE_FIXING
                  std::cout << "### found the alternative:" << std::endl;
                  alternative->dump();
#endif                      
                  changed |= true;
                } else {
#ifdef DEBUG_REFERENCE_FIXING
                  std::cout << "### didn't found an alternative for" << std::endl;
                  operand->dump();
#endif
                  std::cerr << "Could not find a dominating alternative variable." << std::endl;
                  abort();
              }
            }
        }
    }
  return changed;
}

bool
WorkitemReplication::ProcessFunction(Function &F)
{
  Module *M = F.getParent();

  CheckLocalSize(&F);

  // Allocate space for workitem reference maps. Workitem 0 does
  // not need it.
  unsigned workitem_count = LocalSizeZ * LocalSizeY * LocalSizeX;

  BasicBlockVector original_bbs;
  for (Function::iterator i = F.begin(), e = F.end(); i != e; ++i) {
    if (!block_has_barrier(i))
      original_bbs.push_back(i);
  }


  Kernel *K = cast<Kernel> (&F);
  ParallelRegion::ParallelRegionVector* original_parallel_regions =
    K->getParallelRegions(LI);

  std::vector<SmallVector<ParallelRegion *, 8> > parallel_regions(workitem_count);

  parallel_regions[0] = *original_parallel_regions;
  
  // Measure the required context (variables alive in more than one region).
  TargetData &TD = getAnalysis<TargetData>();

  for (SmallVector<ParallelRegion *, 8>::iterator
         i = original_parallel_regions->begin(), e = original_parallel_regions->end();
       i != e; ++i) {
    ParallelRegion *pr = (*i);
    
    for (ParallelRegion::iterator i2 = pr->begin(), e2 = pr->end();
         i2 != e2; ++i2) {
      BasicBlock *bb = (*i2);
      
      for (BasicBlock::iterator i3 = bb->begin(), e3 = bb->end();
           i3 != e3; ++i3) {
        for (Value::use_iterator i4 = i3->use_begin(), e4 = i3->use_end();
             i4 != e4; ++i4) {
          // Instructions can only be used by instructions.
          Instruction *user = cast<Instruction> (*i4);
          
          if (find (pr->begin(), pr->end(), user->getParent()) ==
              pr->end()) {
            // User is not in the defining region.
            ++ContextValues;
            ContextSize += TD.getTypeAllocSize(i3->getType());
            break;
          }
        }
      }
    }
  }

  // Then replicate the ParallelRegions.  
  ValueToValueMapTy *const reference_map = new ValueToValueMapTy[workitem_count - 1];
  for (int z = 0; z < LocalSizeZ; ++z) {
    for (int y = 0; y < LocalSizeY; ++y) {
      for (int x = 0; x < LocalSizeX ; ++x) {
              
        int index = 
          (LocalSizeY * LocalSizeX * z + LocalSizeX * y + x);
	  
        if (index == 0)
          continue;
	  
        std::size_t regionCounter = 0;
        for (SmallVector<ParallelRegion *, 8>::iterator
               i = original_parallel_regions->begin(), 
               e = original_parallel_regions->end();
             i != e; ++i) {
          ParallelRegion *original = (*i);
          ParallelRegion *replicated =
            original->replicate
            (reference_map[index - 1],
             (".wi_" + Twine(x) + "_" + Twine(y) + "_" + Twine(z)));
          if (AddWIMetadata)
            replicated->setID(M->getContext(), x, y, z, regionCounter);
          regionCounter++;
          parallel_regions[index].push_back(replicated);
#ifdef DEBUG_PR_REPLICATION
          std::cerr << "### new replica:" << std::endl;
          replicated->dump();
#endif
        }
      }
    }
  }
  if (AddWIMetadata) {
    std::size_t regionCounter = 0;
    for (SmallVector<ParallelRegion *, 8>::iterator
          i = original_parallel_regions->begin(), 
           e = original_parallel_regions->end();
        i != e; ++i) {
      ParallelRegion *original = (*i);  
      original->setID(M->getContext(), 0,0,0, regionCounter);
      regionCounter++;
    }
  }  
  
  for (int z = 0; z < LocalSizeZ; ++z) {
    for (int y = 0; y < LocalSizeY; ++y) {
      for (int x = 0; x < LocalSizeX ; ++x) {
	  
        int index = 
          (LocalSizeY * LocalSizeX * z + LocalSizeX * y + x);
        
        for (unsigned i = 0, e = parallel_regions[index].size(); i != e; ++i) {
          ParallelRegion *region = parallel_regions[index][i];
          if (index != 0) {
            region->remap(reference_map[index - 1]);
            region->chainAfter(parallel_regions[index - 1][i]);
            region->purge();
          }
          region->insertPrologue(x, y, z);
        }
      }
    }
  }
    
  // Try to merge all workitem first block of each region
  // together (for PHI predecessor correctness).
  for (int z = LocalSizeZ - 1; z >= 0; --z) {
    for (int y = LocalSizeY - 1; y >= 0; --y) {
      for (int x = LocalSizeX - 1; x >= 0; --x) {
          
        int index = 
          (LocalSizeY * LocalSizeX * z + LocalSizeX * y + x);

        if (index == 0)
          continue;
          
        for (unsigned i = 0, e = parallel_regions[index].size(); i != e; ++i) {
          ParallelRegion *region = parallel_regions[index][i];
          BasicBlock *entry = region->entryBB();

          assert (entry != NULL);
          BasicBlock *pred = entry->getUniquePredecessor();
          assert (pred != NULL && "No unique predecessor.");
#ifdef DEBUG_BB_MERGING
          std::cerr << "### pred before merge into predecessor " << std::endl;
          pred->dump();
          std::cerr << "### entry before merge into predecessor " << std::endl;
          entry->dump();
#endif
          movePhiNodes(entry, pred);
        }
      }
    }
  }

  // Add the suffixes to original (wi_0_0_0) basic blocks.
  for (BasicBlockVector::iterator i = original_bbs.begin(),
         e = original_bbs.end();
       i != e; ++i)
    (*i)->setName((*i)->getName() + ".wi_0_0_0");

  // Initialize local size (done at the end to avoid unnecessary
  // replication).
  IRBuilder<> builder(F.getEntryBlock().getFirstNonPHI());

  GlobalVariable *gv;

  int size_t_width = 32;
  if (M->getPointerSize() == llvm::Module::Pointer64)
    size_t_width = 64;

  gv = M->getGlobalVariable("_local_size_x");
  if (gv != NULL)
    builder.CreateStore
      (ConstantInt::get
       (IntegerType::get(M->getContext(), size_t_width),
        LocalSizeX), gv);
  gv = M->getGlobalVariable("_local_size_y");

  if (gv != NULL)
    builder.CreateStore
      (ConstantInt::get
       (IntegerType::get(M->getContext(), size_t_width),
        LocalSizeY), gv);
  gv = M->getGlobalVariable("_local_size_z");

  if (gv != NULL)
    builder.CreateStore
      (ConstantInt::get
       (IntegerType::get(M->getContext(), size_t_width),
        LocalSizeZ), gv);

  delete [] reference_map;

  return true;
}

/**
 * Moves the phi nodes in the beginning of the src to the beginning of
 * the dst. 
 *
 * MergeBlockIntoPredecessor function from llvm discards the phi nodes
 * of the replicated BB because it has only one entry.
 */
void
WorkitemReplication::movePhiNodes(llvm::BasicBlock* src, llvm::BasicBlock* dst) 
{
  while (PHINode *PN = dyn_cast<PHINode>(src->begin())) 
    PN->moveBefore(dst->getFirstNonPHI());
}

void
WorkitemReplication::CheckLocalSize(Function *F)
{
  Module *M = F->getParent();
  
  LocalSizeX = LocalSize[0];
  LocalSizeY = LocalSize[1];
  LocalSizeZ = LocalSize[2];
  
  NamedMDNode *size_info = M->getNamedMetadata("opencl.kernel_wg_size_info");
  if (size_info) {
    for (unsigned i = 0, e = size_info->getNumOperands(); i != e; ++i) {
      MDNode *KernelSizeInfo = size_info->getOperand(i);
      if (KernelSizeInfo->getOperand(0) == F) {
        LocalSizeX = (cast<ConstantInt>(KernelSizeInfo->getOperand(1)))->getLimitedValue();
        LocalSizeY = (cast<ConstantInt>(KernelSizeInfo->getOperand(2)))->getLimitedValue();
        LocalSizeZ = (cast<ConstantInt>(KernelSizeInfo->getOperand(3)))->getLimitedValue();
      }
    }
  }
}

static bool
block_has_barrier(const BasicBlock *bb)
{
  for (BasicBlock::const_iterator i = bb->begin(), e = bb->end();
       i != e; ++i) {
    if (isa<Barrier>(i))
      return true;
  }
  return false;
}
