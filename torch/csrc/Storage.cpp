#include <torch/csrc/python_headers.h>
#ifdef _MSC_VER
#include <c10/util/win32-headers.h>
#endif
#include <structmember.h>

#include <ATen/mps/MPSDevice.h>
#include <c10/core/CPUAllocator.h>
#include <libshm.h>
#include <torch/csrc/CudaIPCTypes.h>
#include <torch/csrc/Device.h>
#include <torch/csrc/DynamicTypes.h>
#include <torch/csrc/StorageMethods.h>
#include <torch/csrc/StorageSharing.h>
#include <torch/csrc/THP.h>
#include <torch/csrc/autograd/utils/wrap_outputs.h>
#include <torch/csrc/copy_utils.h>
#include <torch/csrc/utils/pyobject_preservation.h>
#include <torch/csrc/utils/python_arg_parser.h>

#include <c10/util/intrusive_ptr.h>
#include <fmt/format.h>

template <>
void THPPointer<c10::StorageImpl>::free() {
  if (ptr) {
    c10::raw::intrusive_ptr::decref(ptr);
  }
}

PyTypeObject* THPStorageClass = nullptr;

PyObject* THPStorage_NewWithStorage(
    PyTypeObject* type,
    c10::Storage _storage,
    c10::impl::PyInterpreterStatus status,
    bool allow_preexisting_pyobj) {
  TORCH_CHECK(
      PyType_IsSubtype(type, &THPStorageType),
      "Creating a Storage subclass from a class that does not inherit from ",
      "Storage is not possible. Make sure your class inherits from Storage.");

  auto maybe_pyobj = _storage.unsafeGetStorageImpl()->pyobj_slot()->check_pyobj(
      getPyInterpreter());
  if (maybe_pyobj.has_value() && maybe_pyobj.value()) {
    TORCH_CHECK(
        allow_preexisting_pyobj,
        "Creating a new Storage subclass ",
        type->tp_name,
        " but the raw Storage object is already associated to a python object ",
        "of type ",
        maybe_pyobj.value()->ob_type->tp_name);
    PyObject* obj = *maybe_pyobj;
    PyTypeObject* obj_type = Py_TYPE(obj);
    TORCH_CHECK(
        obj_type == type || PyType_IsSubtype(obj_type, type),
        "Creating a new Storage subclass ",
        type->tp_name,
        " but the raw Storage object is already associated to a python object ",
        "of type ",
        maybe_pyobj.value()->ob_type->tp_name,
        " which is not a subclass of the "
        "requested type");
    return THPStorage_Wrap(std::move(_storage));
  }

  PyObject* obj = type->tp_alloc(type, 0);
  TORCH_CHECK(obj, "Failed to allocate a ", type->tp_name, " object");

  auto s = (THPStorage*)obj;

  // TODO: Is this `new` necessary? It seems to work without it, but
  // THPVariable has it
  new (&s->cdata) c10::MaybeOwned<c10::Storage>();

  s->cdata = c10::MaybeOwned<c10::Storage>::owned(std::move(_storage));

  if (!c10::impl::HermeticPyObjectTLS::get_state()) {
    const auto& storage = THPStorage_Unpack(s);
    storage.unsafeGetStorageImpl()->pyobj_slot()->init_pyobj(
        getPyInterpreter(), obj, status);
  }

  return obj;
}

// Wraps the c10::Storage with a storage PyObject
PyObject* THPStorage_Wrap(c10::Storage storage) {
  if (c10::impl::HermeticPyObjectTLS::get_state()) {
    return THPStorage_NewWithStorage(
        THPStorageClass,
        std::move(storage),
        c10::impl::PyInterpreterStatus::DEFINITELY_UNINITIALIZED);
  }
  c10::StorageImpl* storage_impl = storage.unsafeGetStorageImpl();
  c10::optional<PyObject*> maybe_pyobj =
      storage_impl->pyobj_slot()->check_pyobj(getPyInterpreter());
  c10::impl::PyInterpreterStatus status;
  if (maybe_pyobj.has_value()) {
    auto obj = *maybe_pyobj;
    if (obj) {
      if (storage_impl->pyobj_slot()->owns_pyobj()) {
        storage_impl->pyobj_slot()->set_owns_pyobj(false);
        reinterpret_cast<THPStorage*>(obj)->cdata =
            c10::MaybeOwned<c10::Storage>::owned(std::move(storage));
        return obj;
      } else {
        Py_INCREF(obj);
        return obj;
      }
    }
    status = c10::impl::PyInterpreterStatus::TAGGED_BY_US;
  } else {
    if (storage.use_count() <= 1) {
      status = c10::impl::PyInterpreterStatus::DEFINITELY_UNINITIALIZED;
    } else {
      status = c10::impl::PyInterpreterStatus::MAYBE_UNINITIALIZED;
    }
  }
  return THPStorage_NewWithStorage(THPStorageClass, std::move(storage), status);
}

static bool THPStorage_isPreservable(THPStorage* self) {
  if (self->cdata.unsafeIsBorrowed()) {
    return false;
  }
  auto const& storage = THPStorage_Unpack(self);

  if (storage.unsafeGetStorageImpl()->pyobj_slot()->check_pyobj(
          getPyInterpreter()) != c10::make_optional((PyObject*)self)) {
    return false;
  }
  if (storage.use_count() <= 1) {
    return false;
  }
  return true;
}

static bool THPStorage_tryPreserve(THPStorage* self) {
  const auto& storage = THPStorage_Unpack(self);

  if (!THPStorage_isPreservable(self)) {
    return false;
  }

  c10::StorageImpl* storage_impl = storage.unsafeGetStorageImpl();

  TORCH_INTERNAL_ASSERT(!storage_impl->pyobj_slot()->owns_pyobj());

  storage_impl->pyobj_slot()->set_owns_pyobj(true);
  Py_INCREF(self);

  self->cdata = c10::MaybeOwned<c10::Storage>::borrowed(storage);
  return true;
}

static void THPStorage_subclass_dealloc(PyObject* self) {
  THPStorage* _self = (THPStorage*)self;

  if (THPStorage_tryPreserve(_self)) {
    return;
  }

  // Some subclass of StorageBase could be GC-tracked objects even
  // though the base class is not
  auto* type = Py_TYPE(self);
  if (PyType_HasFeature(type, Py_TPFLAGS_HAVE_GC) != 0) {
    PyObject_GC_UnTrack(self);
  }

  bool has_finalizer = type->tp_finalize || type->tp_del;

  if (type->tp_finalize) {
    PyObject_GC_Track(self);
    if (PyObject_CallFinalizerFromDealloc(self) < 0) {
      // The finalizer has resurrected the PyObject and there is a new Python
      // reference to it, so we can just stop deallocating. Read about
      // resurrection from `__del__` here:
      // https://docs.python.org/3/reference/datamodel.html#object.__del__
      return;
    }
    PyObject_GC_UnTrack(self);
  }

  // base test is unnecessary as THPStorae does not set this
  if (type->tp_weaklistoffset) {
    PyObject_ClearWeakRefs(self);
  }

  if (type->tp_del) {
    PyObject_GC_Track(self);
    type->tp_del(self);
    if (self->ob_refcnt > 0) {
      // Resurrected (see above comment about resurrection from `__del__`)
      return;
    }
    PyObject_GC_UnTrack(self);
  }

  if (has_finalizer) {
    /* New weakrefs could be created during the finalizer call.
       If this occurs, clear them out without calling their
       finalizers since they might rely on part of the object
       being finalized that has already been destroyed. */
    if (type->tp_weaklistoffset) {
      /* Modeled after GET_WEAKREFS_LISTPTR() */
      PyWeakReference** list =
          (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(self);
      while (*list)
        _PyWeakref_ClearRef(*list);
    }
  }

  // Clear slots
  {
    PyTypeObject* base = type;
    while (base != &THPStorageType) {
      if (Py_SIZE(base)) {
        clear_slots(base, self);
      }
      base = base->tp_base;
      TORCH_INTERNAL_ASSERT(base);
    }
  }

  // Clear __dict__
  if (C10_LIKELY(type->tp_dictoffset)) {
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr != nullptr) {
      PyObject* dict = *dictptr;
      if (dict != nullptr) {
        Py_DECREF(dict);
        *dictptr = nullptr;
      }
    }
  }

  TORCH_INTERNAL_ASSERT(Py_TYPE(self) == type);

  _self->cdata.~MaybeOwned<c10::Storage>();
  Py_TYPE(_self)->tp_free(self);

  TORCH_INTERNAL_ASSERT(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
  Py_DECREF(type);
}

static PyObject* THPStorage_pynew(
    PyTypeObject* type,
    PyObject* args,
    PyObject* kwargs) {
  HANDLE_TH_ERRORS
  TORCH_CHECK(
      type != &THPStorageType,
      "Cannot directly construct StorageBase; subclass it and then construct that");
  static torch::PythonArgParser parser({
      THPStorageStr "(*, int64_t allocator=None, Device device=None)",
      THPStorageStr
      "(int64_t size, *, int64_t allocator=None, Device device=None)",
      THPStorageStr
      "(PyObject* sequence, *, int64_t allocator=None, Device device=None)",
  });
  torch::ParsedArgs<3> parsed_args;
  auto r = parser.parse(args, kwargs, parsed_args);

  int64_t allocator_arg_idx = 0;
  int64_t device_arg_idx = 1;

  if (r.idx > 0) {
    allocator_arg_idx = 1;
    device_arg_idx = 2;
  }

  c10::optional<int64_t> allocator_opt = r.toInt64Optional(allocator_arg_idx);
  c10::optional<at::Device> device_opt = r.deviceOptional(device_arg_idx);

  TORCH_CHECK(
      !allocator_opt.has_value() || !device_opt.has_value(),
      THPStorageStr,
      "(): only one or neither of 'allocator' or 'device' can ",
      "be given, but not both");

  PyObject* self = nullptr;
  c10::Allocator* allocator = nullptr;
  at::OptionalDeviceGuard device_guard;

  if (allocator_opt.has_value()) {
    allocator = reinterpret_cast<c10::Allocator*>(allocator_opt.value());
  } else if (device_opt.has_value()) {
    at::Device device = device_opt.value();
    if (device.type() == at::kCPU) {
      allocator = c10::GetDefaultCPUAllocator();
#ifdef USE_CUDA
    } else if (device.type() == at::kCUDA) {
      at::globalContext().lazyInitCUDA();
      allocator = c10::cuda::CUDACachingAllocator::get();
#endif
#ifdef USE_MPS
    } else if (device.type() == at::kMPS) {
      allocator = at::mps::GetMPSAllocator();
#endif
    } else if (device.type() == at::DeviceType::XPU) {
      allocator = c10::GetAllocator(device.type());
    } else if (device.type() == at::DeviceType::Meta) {
      allocator = c10::GetAllocator(device.type());
    } else {
      TORCH_CHECK(
          false,
          THPStorageStr,
          "(): Storage device not recognized: ",
          device.type());
    }
    device_guard.reset_device(device);
  } else {
    allocator = c10::GetDefaultCPUAllocator();
  }

  // torch.Storage(*, ...)
  if (r.idx == 0) {
    self = THPStorage_NewWithStorage(
        type,
        c10::make_intrusive<c10::StorageImpl>(
            c10::StorageImpl::use_byte_size_t(),
            0,
            allocator,
            /*resizable=*/true),
        c10::impl::PyInterpreterStatus::DEFINITELY_UNINITIALIZED);

    // torch.Storage(size, *, ...)
  } else if (r.idx == 1) {
    int64_t size = r.toInt64(0);
    self = THPStorage_NewWithStorage(
        type,
        c10::make_intrusive<c10::StorageImpl>(
            c10::StorageImpl::use_byte_size_t(),
            size,
            allocator,
            /*resizable=*/true),
        c10::impl::PyInterpreterStatus::DEFINITELY_UNINITIALIZED);

    // torch.Storage(sequence, *, ...)
  } else if (r.idx == 2) {
    PyObject* sequence = r.pyobject(0);
    Py_ssize_t length = PySequence_Length(sequence);
    TORCH_CHECK(
        PySequence_Check(sequence),
        THPStorageStr,
        "(): Expected a sequence type, but got ",
        THPUtils_typename(sequence));
    TORCH_CHECK(
        length >= 0,
        THPStorageStr,
        "(): Could not obtain the length of sequence of type ",
        THPUtils_typename(sequence));
    self = THPStorage_NewWithStorage(
        type,
        c10::make_intrusive<c10::StorageImpl>(
            c10::StorageImpl::use_byte_size_t(),
            length,
            allocator,
            /*resizable=*/true),
        c10::impl::PyInterpreterStatus::DEFINITELY_UNINITIALIZED);
    THPObjectPtr item;
    try {
      const auto& storage = THPStorage_Unpack(self);
      for (Py_ssize_t i = 0; i < length; i++) {
        item = PySequence_GetItem(sequence, i);
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        uint8_t value = THPByteUtils_unpackReal(item.get());
        if (allocator == c10::GetDefaultCPUAllocator()) {
          storage.unsafe_data<uint8_t>()[i] = value;
        } else {
          // TODO: this might be slow - consider batched updates?
          storage_set(storage, i, value);
        }
      }
    } catch (const std::exception& e) {
      THPUtils_setError(
          THPStorageStr
          "(): tried to construct a storage from a sequence (%s), "
          "but one of the items was of type %s instead of int",
          THPUtils_typename(sequence),
          THPUtils_typename(item.get()));
      return nullptr;
    }
  }
  return self;
  Py_RETURN_NONE;
  END_HANDLE_TH_ERRORS
}

static Py_ssize_t THPStorage_length(THPStorage* self) {
  HANDLE_TH_ERRORS
  THPStorage_assertNotNull(self);
  return THPStorage_Unpack(self).nbytes();
  END_HANDLE_TH_ERRORS_RET(-1)
}

static PyObject* THPStorage_get(THPStorage* self, PyObject* index) {
  HANDLE_TH_ERRORS
  THPStorage_assertNotNull(self);
  const auto& storage = THPStorage_Unpack(self);
  /* Integer index */
  if (THPUtils_checkLong(index)) {
    int64_t nindex = THPUtils_unpackLong(index);
    if (nindex < 0)
      nindex += storage.nbytes();
    if (nindex < 0 || nindex >= static_cast<int64_t>(storage.nbytes())) {
      PyErr_SetString(
          PyExc_IndexError,
          fmt::format(
              "index {} out of range for storage of size {}",
              nindex,
              storage.nbytes()));
      return nullptr;
    }
    uint8_t value = storage_get(storage, nindex);
    return THPByteUtils_newReal(value);
    /* Slice index */
  } else if (PySlice_Check(index)) {
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    Py_ssize_t start, stop, slicelength, step;
    int64_t len = storage.nbytes();
    if (PySlice_GetIndicesEx(index, len, &start, &stop, &step, &slicelength) !=
        0)
      return nullptr;
    if (step != 1) {
      THPUtils_setError(
          "Trying to slice with a step of %lld, but only a step of "
          "1 is supported",
          (long long)step);
      return nullptr;
    }

    const auto& storage = THPStorage_Unpack(self);
    uint8_t* data = storage.data<uint8_t>();

    at::StorageImpl* old_storage_impl = storage.unsafeGetStorageImpl();
    c10::raw::intrusive_ptr::incref(old_storage_impl);
    auto new_storage_impl = c10::make_intrusive<at::StorageImpl>(
        c10::StorageImpl::use_byte_size_t(),
#ifdef THQUANTIZED
        slicelength * sizeof(quantized_t),
#else
        slicelength,
#endif
        at::DataPtr(
            static_cast<void*>(data + start),
            old_storage_impl,
            [](void* s) {
              c10::raw::intrusive_ptr::decref(static_cast<at::StorageImpl*>(s));
            },
            old_storage_impl->device()),
        old_storage_impl->allocator(),
        /* resizable */ false);

    PyObject* _ret = THPStorage_NewWithStorage(
        Py_TYPE(self),
        std::move(new_storage_impl),
        c10::impl::PyInterpreterStatus::DEFINITELY_UNINITIALIZED);

    return _ret;
  }
  PyErr_Format(
      PyExc_TypeError,
      "can't index a " THPStorageStr " with %s",
      THPUtils_typename(index));
  return nullptr;
  END_HANDLE_TH_ERRORS
}

static int THPStorage_set(THPStorage* self, PyObject* index, PyObject* value) {
  HANDLE_TH_ERRORS
  THPStorage_assertNotNull(self);
  if (!THPByteUtils_checkReal(value)) {
    THPUtils_setError(
        "can only set storage content with a int types, but got "
        "%s instead",
        THPUtils_typename(value));
    return -1;
  }

  uint8_t rvalue = THPByteUtils_unpackReal(value);
  const auto& storage = THPStorage_Unpack(self);
  if (THPUtils_checkLong(index)) {
    int64_t nindex = THPUtils_unpackLong(index);
    storage_set(storage, nindex, rvalue);
    return 0;
  } else if (PySlice_Check(index)) {
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    Py_ssize_t start, stop, slicelength, step;
    int64_t len = storage.nbytes();
    if (PySlice_GetIndicesEx(index, len, &start, &stop, &step, &slicelength) !=
        0)
      return -1;
    if (step != 1) {
      THPUtils_setError(
          "Trying to slice with a step of %lld, but only a step of "
          "1 is supported",
          (long long)step);
      return 0;
    }
    // TODO: check the bounds only once
    // TODO: fill?
    for (; start < stop; start++)
      storage_set(storage, start, rvalue);
    return 0;
  }
  THPUtils_setError(
      "can't index a " THPStorageStr " with %s", THPUtils_typename(index));
  return -1;
  END_HANDLE_TH_ERRORS_RET(-1)
}

static PyMappingMethods THPStorage_mappingmethods = {
    (lenfunc)THPStorage_length,
    (binaryfunc)THPStorage_get,
    (objobjargproc)THPStorage_set};

struct THPStorageMeta {
  PyHeapTypeObject base;
};

int THPStorageMetaType_init(PyObject* cls, PyObject* args, PyObject* kwargs);

PyTypeObject THPStorageMetaType = {
    PyVarObject_HEAD_INIT(
        DEFERRED_ADDRESS(&PyType_Type),
        0) "torch._C._StorageMeta", /* tp_name */
    sizeof(THPStorageMeta), /* tp_basicsize */
    0, /* tp_itemsize */
    nullptr, /* tp_dealloc */
    0, /* tp_vectorcall_offset */
    nullptr, /* tp_getattr */
    nullptr, /* tp_setattr */
    nullptr, /* tp_reserved */
    nullptr, /* tp_repr */
    nullptr, /* tp_as_number */
    nullptr, /* tp_as_sequence */
    nullptr, /* tp_as_mapping */
    nullptr, /* tp_hash  */
    nullptr, /* tp_call */
    nullptr, /* tp_str */
    nullptr, /* tp_getattro */
    nullptr, /* tp_setattro */
    nullptr, /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    nullptr, /* tp_doc */
    nullptr, /* tp_traverse */
    nullptr, /* tp_clear */
    nullptr, /* tp_richcompare */
    0, /* tp_weaklistoffset */
    nullptr, /* tp_iter */
    nullptr, /* tp_iternext */
    nullptr, /* tp_methods */
    nullptr, /* tp_members */
    nullptr, /* tp_getset */
    DEFERRED_ADDRESS(&PyType_Type), /* tp_base */
    nullptr, /* tp_dict */
    nullptr, /* tp_descr_get */
    nullptr, /* tp_descr_set */
    0, /* tp_dictoffset */
    THPStorageMetaType_init, /* tp_init */
    nullptr, /* tp_alloc */
    nullptr, /* tp_new */
};

// TODO: implement equality
PyTypeObject THPStorageType = {
    PyVarObject_HEAD_INIT(
        &THPStorageMetaType,
        0) "torch._C.StorageBase", /* tp_name */
    sizeof(THPStorage), /* tp_basicsize */
    0, /* tp_itemsize */
    nullptr, /* tp_dealloc */
    0, /* tp_vectorcall_offset */
    nullptr, /* tp_getattr */
    nullptr, /* tp_setattr */
    nullptr, /* tp_reserved */
    nullptr, /* tp_repr */
    nullptr, /* tp_as_number */
    nullptr, /* tp_as_sequence */
    &THPStorage_mappingmethods, /* tp_as_mapping */
    nullptr, /* tp_hash  */
    nullptr, /* tp_call */
    nullptr, /* tp_str */
    nullptr, /* tp_getattro */
    nullptr, /* tp_setattro */
    nullptr, /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    nullptr, /* tp_doc */
    nullptr, /* tp_traverse */
    nullptr, /* tp_clear */
    nullptr, /* tp_richcompare */
    0, /* tp_weaklistoffset */
    nullptr, /* tp_iter */
    nullptr, /* tp_iternext */
    nullptr,
    /* will be assigned in init */ /* tp_methods */
    nullptr,
    /* will be assigned in init */ /* tp_members */
    nullptr, /* tp_getset */
    nullptr, /* tp_base */
    nullptr, /* tp_dict */
    nullptr, /* tp_descr_get */
    nullptr, /* tp_descr_set */
    0, /* tp_dictoffset */
    nullptr, /* tp_init */
    nullptr, /* tp_alloc */
    THPStorage_pynew, /* tp_new */
};

int THPStorageMetaType_init(PyObject* cls, PyObject* args, PyObject* kwargs) {
  if (PyType_Type.tp_init(cls, args, kwargs) < 0) {
    return -1;
  }
  ((PyTypeObject*)cls)->tp_dealloc = (destructor)THPStorage_subclass_dealloc;
  return 0;
}

static PyObject* THPStorage_device(THPStorage* self, void* unused) {
  HANDLE_TH_ERRORS
  THPStorage_assertNotNull(self);
  return THPDevice_New(THPStorage_Unpack(self).device());
  END_HANDLE_TH_ERRORS
}

PyObject* THPStorage_get_cdata(THPStorage* self, void* unused) {
  HANDLE_TH_ERRORS
  return PyLong_FromVoidPtr(THPStorage_Unpack(self).unsafeGetStorageImpl());
  END_HANDLE_TH_ERRORS
}

typedef PyObject* (*getter)(PyObject*, void*);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables)
static struct PyGetSetDef THPStorage_properties[] = {
    {"device", (getter)THPStorage_device, nullptr, nullptr, nullptr},
    {"_cdata", (getter)THPStorage_get_cdata, nullptr, nullptr, nullptr},
    {nullptr}};

bool THPStorage_init(PyObject* module) {
  static std::vector<PyMethodDef> methods;
  THPUtils_addPyMethodDefs(methods, THPStorage_getMethods());
  THPUtils_addPyMethodDefs(methods, THPStorage_getSharingMethods());

  THPStorageMetaType.tp_base = &PyType_Type;
  if (PyType_Ready(&THPStorageMetaType) < 0)
    return false;
  Py_INCREF(&THPStorageMetaType);
  PyModule_AddObject(module, "_StorageMeta", (PyObject*)&THPStorageMetaType);

  THPStorageType.tp_methods = methods.data();
  THPStorageType.tp_getset = THPStorage_properties;
  if (PyType_Ready(&THPStorageType) < 0)
    return false;
  Py_INCREF(&THPStorageType);
  PyModule_AddObject(module, "StorageBase", (PyObject*)&THPStorageType);
  return true;
}

void THPStorage_postInit(PyObject* module) {
  THPStorageClass =
      (PyTypeObject*)PyObject_GetAttrString(module, "UntypedStorage");
  if (!THPStorageClass)
    throw python_error();
}

void THPStorage_assertNotNull(THPStorage* storage) {
  TORCH_CHECK(
      THPStorage_Unpack(storage).unsafeGetStorageImpl(), "Got a null Storage");
}

void THPStorage_assertNotNull(PyObject* obj) {
  THPStorage_assertNotNull((THPStorage*)obj);
}
