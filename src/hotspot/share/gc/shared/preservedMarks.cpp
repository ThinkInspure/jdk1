/*
 * Copyright (c) 2016, 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shared/workgroup.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/macros.hpp"

void PreservedMarks::restore() {
  while (!_stack.is_empty()) {
    const OopAndMarkWord elem = _stack.pop();
    elem.set_mark();
  }
  assert_empty();
}

void PreservedMarks::adjust_during_full_gc() {
  StackIterator<OopAndMarkWord, mtGC> iter(_stack);
  while (!iter.is_empty()) {
    OopAndMarkWord* elem = iter.next_addr();

    oop obj = elem->get_oop();
    if (obj->is_forwarded()) {
      elem->set_oop(obj->forwardee());
    }
  }
}

void PreservedMarks::restore_and_increment(volatile size_t* const total_size_addr) {
  const size_t stack_size = size();
  restore();
  // Only do the atomic add if the size is > 0.
  if (stack_size > 0) {
    Atomic::add(total_size_addr, stack_size);
  }
}

#ifndef PRODUCT
void PreservedMarks::assert_empty() {
  assert(_stack.is_empty(), "stack expected to be empty, size = " SIZE_FORMAT,
         _stack.size());
  assert(_stack.cache_size() == 0,
         "stack expected to have no cached segments, cache size = " SIZE_FORMAT,
         _stack.cache_size());
}
#endif // ndef PRODUCT

void RemoveForwardedPointerClosure::do_object(oop obj) {
  if (obj->is_forwarded()) {
    PreservedMarks::init_forwarded_mark(obj);
  }
}

void PreservedMarksSet::init(uint num) {
  assert(_stacks == NULL && _num == 0, "do not re-initialize");
  assert(num > 0, "pre-condition");
  if (_in_c_heap) {
    _stacks = NEW_C_HEAP_ARRAY(Padded<PreservedMarks>, num, mtGC);
  } else {
    _stacks = NEW_RESOURCE_ARRAY(Padded<PreservedMarks>, num);
  }
  for (uint i = 0; i < num; i += 1) {
    ::new (_stacks + i) PreservedMarks();
  }
  _num = num;

  assert_empty();
}

class ParRestoreTask : public AbstractGangTask {
private:
  PreservedMarksSet* const _preserved_marks_set;
  SequentialSubTasksDone _sub_tasks;
  volatile size_t* const _total_size_addr;

public:
  virtual void work(uint worker_id) {
    uint task_id = 0;
    while (_sub_tasks.try_claim_task(/* reference */ task_id)) {
      _preserved_marks_set->get(task_id)->restore_and_increment(_total_size_addr);
    }
    _sub_tasks.all_tasks_completed();
  }

  ParRestoreTask(uint worker_num,
                 PreservedMarksSet* preserved_marks_set,
                 volatile size_t* total_size_addr)
      : AbstractGangTask("Parallel Preserved Mark Restoration"),
        _preserved_marks_set(preserved_marks_set),
        _total_size_addr(total_size_addr) {
    _sub_tasks.set_n_threads(worker_num);
    _sub_tasks.set_n_tasks(preserved_marks_set->num());
  }
};

void PreservedMarksSet::restore(WorkGang* workers) {
  volatile size_t total_size = 0;

#ifdef ASSERT
  // This is to make sure the total_size we'll calculate below is correct.
  size_t total_size_before = 0;
  for (uint i = 0; i < _num; i += 1) {
    total_size_before += get(i)->size();
  }
#endif // def ASSERT

  if (workers == NULL) {
    for (uint i = 0; i < num(); i += 1) {
      total_size += get(i)->size();
      get(i)->restore();
    }
  } else {
    ParRestoreTask task(workers->active_workers(), this, &total_size);
    workers->run_task(&task);
  }

  assert_empty();

  assert(total_size == total_size_before,
         "total_size = " SIZE_FORMAT " before = " SIZE_FORMAT,
         total_size, total_size_before);

  log_trace(gc)("Restored " SIZE_FORMAT " marks", total_size);
}

void PreservedMarksSet::reclaim() {
  assert_empty();

  for (uint i = 0; i < _num; i += 1) {
    _stacks[i].~Padded<PreservedMarks>();
  }

  if (_in_c_heap) {
    FREE_C_HEAP_ARRAY(Padded<PreservedMarks>, _stacks);
  } else {
    // the array was resource-allocated, so nothing to do
  }
  _stacks = NULL;
  _num = 0;
}

#ifndef PRODUCT
void PreservedMarksSet::assert_empty() {
  assert(_stacks != NULL && _num > 0, "should have been initialized");
  for (uint i = 0; i < _num; i += 1) {
    get(i)->assert_empty();
  }
}
#endif // ndef PRODUCT
