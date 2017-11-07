//------------------------------------------------------------------------------
//  Copyright 2017 H2O.ai
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//------------------------------------------------------------------------------
#include "column.h"

PyObjectColumn::PyObjectColumn() : FwColumn<PyObject*>() {}

PyObjectColumn::PyObjectColumn(int64_t nrows_, MemoryBuffer* mb) :
    FwColumn<PyObject*>(nrows_, mb) {}


PyObjectColumn::~PyObjectColumn() {
  // PyObject** elems = elements();
  // for (int64_t i = 0; i < nrows; i++) {
  //   Py_DECREF(elems[i]);
  // }
  // delete mbuf;
}

SType PyObjectColumn::stype() const {
  return ST_OBJECT_PYPTR;
}



//----- Type casts -------------------------------------------------------------

void PyObjectColumn::cast_into(PyObjectColumn* target) const {
  PyObject** src_data = this->elements();
  for (int64_t i = 0; i < this->nrows; ++i) {
    Py_XINCREF(src_data[i]);
  }
  memcpy(target->data(), this->data(), alloc_size());
}


