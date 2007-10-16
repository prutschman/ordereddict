/* Ordered Dictionary object implementation using a hash table and a vector of
   pointers to the items.
*/
/*

  This file has been directly derived from and retains many algorithms from
  objectdict.c in the Python 2.5.1 source distribution. Its licensing therefore 
  is governed by the license as distributed with Python 2.5.1 available in the
  file LICNESE in the source distribution of ordereddict
  
  Copyright (c) 2001, 2002, 2003, 2004, 2005, 2006, 2007  Python Software 
  Foundation; All Rights Reserved"

  2007-10-13: Anthon van der Neut
*/

/*
Ordering by key insertion order (KIO) instead of key/val insertion order
(KVIO) is less expensive  (as the list of keys does not have to be updated).
*/

#include "Python.h"
#include "ordereddict.h"

#if PY_VERSION_HEX < 0x02060000
#define Py_Type(ob)            (((PyObject*)(ob))->ob_type)
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#define DEFERRED_ADDRESS(ADDR) 0

/* Set a key error with the specified argument, wrapping it in a
 * tuple automatically so that tuple keys are not unpacked as the
 * exception arguments. */
static void
set_key_error(PyObject *arg)
{
	PyObject *tup;
	tup = PyTuple_Pack(1, arg);
	if (!tup)
		return; /* caller will expect error to be set anyway */
	PyErr_SetObject(PyExc_KeyError, tup);
	Py_DECREF(tup);
}

/* Define this out if you don't want conversion statistics on exit. */
#undef SHOW_CONVERSION_COUNTS

/* See large comment block below.  This must be >= 1. */
#define PERTURB_SHIFT 5

/*
see object/dictobject.c for subtilities of the base dict implementation
*/

/* Object used as dummy key to fill deleted entries */
static PyObject *dummy = NULL; /* Initialized by first call to newPyDictObject() */

#ifdef Py_REF_DEBUG
PyObject *
_PyDict_Dummy(void)
{
	return dummy;
}
#endif

/* forward declarations */
static PyOrderedDictEntry *
lookdict_string(PyOrderedDictObject *mp, PyObject *key, long hash);

#ifdef SHOW_CONVERSION_COUNTS
static long created = 0L;
static long converted = 0L;

static void
show_counts(void)
{
	fprintf(stderr, "created %ld string dicts\n", created);
	fprintf(stderr, "converted %ld to normal dicts\n", converted);
	fprintf(stderr, "%.2f%% conversion rate\n", (100.0*converted)/created);
}
#endif

/* Initialization macros.
   There are two ways to create a dict:  PyOrderedDict_New() is the main C API
   function, and the tp_new slot maps to dict_new().  In the latter case we
   can save a little time over what PyOrderedDict_New does because it's guaranteed
   that the PyOrderedDictObject struct is already zeroed out.
   Everyone except dict_new() should use EMPTY_TO_MINSIZE (unless they have
   an excellent reason not to).
*/

#define INIT_NONZERO_DICT_SLOTS(mp) do {				\
	(mp)->ma_table = (mp)->ma_smalltable;				\
	(mp)->ma_otablep = (mp)->ma_smallotablep;           \
	(mp)->ma_mask = PyOrderedDict_MINSIZE - 1;				\
    } while(0)

#define EMPTY_TO_MINSIZE(mp) do {					\
	memset((mp)->ma_smalltable, 0, sizeof((mp)->ma_smalltable));	\
	memset((mp)->ma_smallotablep, 0, sizeof((mp)->ma_smallotablep));	\
	(mp)->ma_used = (mp)->ma_fill = 0;				\
	INIT_NONZERO_DICT_SLOTS(mp);					\
    } while(0)

/* Dictionary reuse scheme to save calls to malloc, free, and memset */
#define MAXFREEDICTS 80
static PyOrderedDictObject *free_dicts[MAXFREEDICTS];
static int num_free_dicts = 0;

PyObject *
PyOrderedDict_New(void)
{
	register PyOrderedDictObject *mp;
	assert(dummy != NULL);
	if (num_free_dicts) {
		mp = free_dicts[--num_free_dicts];
		assert (mp != NULL);
		/* assert (mp->ob_type == &PyOrderedDict_Type); */
		assert (Py_Type(mp) == &PyOrderedDict_Type);
		_Py_NewReference((PyObject *)mp);
		if (mp->ma_fill) {
			EMPTY_TO_MINSIZE(mp);
		}
		assert (mp->ma_used == 0);
		assert (mp->ma_table == mp->ma_smalltable);
		assert (mp->ma_otablep == mp->ma_smallotablep);
		assert (mp->ma_mask == PyOrderedDict_MINSIZE - 1);
	} else {
		mp = PyObject_GC_New(PyOrderedDictObject, &PyOrderedDict_Type);
		if (mp == NULL)
			return NULL;
		EMPTY_TO_MINSIZE(mp);
	}
	mp->ma_lookup = lookdict_string;
#ifdef SHOW_CONVERSION_COUNTS
	++created;
#endif
#ifndef _WIN32
     /* AvdN: FIXME this gives a linker error on Win XP with VS2003
	 on not being able to find __PyGC_generation0 for linking 
	 */
	_PyObject_GC_TRACK(mp);
#endif
	return (PyObject *)mp;
}

/*
The basic lookup function used by all operations.
This is based on Algorithm D from Knuth Vol. 3, Sec. 6.4.
Open addressing is preferred over chaining since the link overhead for
chaining would be substantial (100% with typical malloc overhead).

The initial probe index is computed as hash mod the table size. Subsequent
probe indices are computed as explained earlier.

All arithmetic on hash should ignore overflow.

(The details in this version are due to Tim Peters, building on many past
contributions by Reimer Behrends, Jyrki Alakuijala, Vladimir Marangozov and
Christian Tismer).

lookdict() is general-purpose, and may return NULL if (and only if) a
comparison raises an exception (this was new in Python 2.5).
lookdict_string() below is specialized to string keys, comparison of which can
never raise an exception; that function can never return NULL.  For both, when
the key isn't found a PyOrderedDictEntry* is returned for which the me_value field is
NULL; this is the slot in the dict at which the key would have been found, and
the caller can (if it wishes) add the <key, value> pair to the returned
PyOrderedDictEntry *.
*/
static PyOrderedDictEntry *
lookdict(PyOrderedDictObject *mp, PyObject *key, register long hash)
{
	register size_t i;
	register size_t perturb;
	register PyOrderedDictEntry *freeslot;
	register size_t mask = (size_t)mp->ma_mask;
	PyOrderedDictEntry *ep0 = mp->ma_table;
	register PyOrderedDictEntry *ep;
	register int cmp;
	PyObject *startkey;

	i = (size_t)hash & mask;
	ep = &ep0[i];
	if (ep->me_key == NULL || ep->me_key == key)
		return ep;

	if (ep->me_key == dummy)
		freeslot = ep;
	else {
		if (ep->me_hash == hash) {
			startkey = ep->me_key;
			cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
			if (cmp < 0)
				return NULL;
			if (ep0 == mp->ma_table && ep->me_key == startkey) {
				if (cmp > 0)
					return ep;
			}
			else {
				/* The compare did major nasty stuff to the
				 * dict:  start over.
				 * XXX A clever adversary could prevent this
				 * XXX from terminating.
 				 */
 				return lookdict(mp, key, hash);
 			}
		}
		freeslot = NULL;
	}

	/* In the loop, me_key == dummy is by far (factor of 100s) the
	   least likely outcome, so test for that last. */
	for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
		i = (i << 2) + i + perturb + 1;
		ep = &ep0[i & mask];
		if (ep->me_key == NULL)
			return freeslot == NULL ? ep : freeslot;
		if (ep->me_key == key)
			return ep;
		if (ep->me_hash == hash && ep->me_key != dummy) {
			startkey = ep->me_key;
			cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
			if (cmp < 0)
				return NULL;
			if (ep0 == mp->ma_table && ep->me_key == startkey) {
				if (cmp > 0)
					return ep;
			}
			else {
				/* The compare did major nasty stuff to the
				 * dict:  start over.
				 * XXX A clever adversary could prevent this
				 * XXX from terminating.
 				 */
 				return lookdict(mp, key, hash);
 			}
		}
		else if (ep->me_key == dummy && freeslot == NULL)
			freeslot = ep;
	}
	assert(0);	/* NOT REACHED */
	return 0;
}

/*
 * Hacked up version of lookdict which can assume keys are always strings;
 * this assumption allows testing for errors during PyObject_RichCompareBool()
 * to be dropped; string-string comparisons never raise exceptions.  This also
 * means we don't need to go through PyObject_RichCompareBool(); we can always
 * use _PyString_Eq() directly.
 *
 * This is valuable because dicts with only string keys are very common.
 */
static PyOrderedDictEntry *
lookdict_string(PyOrderedDictObject *mp, PyObject *key, register long hash)
{
	register size_t i;
	register size_t perturb;
	register PyOrderedDictEntry *freeslot;
	register size_t mask = (size_t)mp->ma_mask;
	PyOrderedDictEntry *ep0 = mp->ma_table;
	register PyOrderedDictEntry *ep;

	/* Make sure this function doesn't have to handle non-string keys,
	   including subclasses of str; e.g., one reason to subclass
	   strings is to override __eq__, and for speed we don't cater to
	   that here. */
	if (!PyString_CheckExact(key)) {
#ifdef SHOW_CONVERSION_COUNTS
		++converted;
#endif
		mp->ma_lookup = lookdict;
		return lookdict(mp, key, hash);
	}
	i = hash & mask;
	ep = &ep0[i];
	if (ep->me_key == NULL || ep->me_key == key)
		return ep;
	if (ep->me_key == dummy)
		freeslot = ep;
	else {
		if (ep->me_hash == hash && _PyString_Eq(ep->me_key, key))
			return ep;
		freeslot = NULL;
	}

	/* In the loop, me_key == dummy is by far (factor of 100s) the
	   least likely outcome, so test for that last. */
	for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
		i = (i << 2) + i + perturb + 1;
		ep = &ep0[i & mask];
		if (ep->me_key == NULL)
			return freeslot == NULL ? ep : freeslot;
		if (ep->me_key == key
		    || (ep->me_hash == hash
		        && ep->me_key != dummy
			&& _PyString_Eq(ep->me_key, key)))
			return ep;
		if (ep->me_key == dummy && freeslot == NULL)
			freeslot = ep;
	}
	assert(0);	/* NOT REACHED */
	return 0;
}

/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Eats a reference to key and one to value.
Returns -1 if an error occurred, or 0 on success.
*/
static int
insertdict(register PyOrderedDictObject *mp, PyObject *key, long hash, 
           PyObject *value, Py_ssize_t index)
{
	PyObject *old_value;
	Py_ssize_t oindex;
	register PyOrderedDictEntry *ep, **epp = NULL;
	typedef PyOrderedDictEntry *(*lookupfunc)(PyOrderedDictObject *, PyObject *, long);

	assert(mp->ma_lookup != NULL);
	ep = mp->ma_lookup(mp, key, hash);
	if (ep == NULL) {
		Py_DECREF(key);
		Py_DECREF(value);
		return -1;
	}
	if (ep->me_value != NULL) { /* updating a value */
		old_value = ep->me_value;
		ep->me_value = value;
		if (index >= 0) {
			for (oindex = 0, epp = mp->ma_otablep; oindex < mp->ma_used; 
				oindex++, epp++)
				if (*epp == ep) 
					break;
			/* epp now points to item and oindex is its index (optimize?) */
			/* if index == oindex we don't have to anything */
			/* YYYY */
			if (index < oindex) {
				epp = mp->ma_otablep;
				epp += index;
				memmove(epp + 1, epp, (oindex - index) * sizeof(PyOrderedDictEntry *));
				*epp = ep;
			} else if (index > oindex) {
				memmove(epp, epp + 1, (index - oindex) * sizeof(PyOrderedDictEntry *));
				mp->ma_otablep[index] = ep;
			}
		}
		/* XXX AvdN: here we update key to end of otablep if  KVIO */
		Py_DECREF(old_value); /* which **CAN** re-enter */
		Py_DECREF(key);
	}
	else { /* new value */
		if (ep->me_key == NULL)
			mp->ma_fill++;
		else {
			assert(ep->me_key == dummy);
			Py_DECREF(dummy);
		}
		ep->me_key = key;
		ep->me_hash = (Py_ssize_t)hash;
		ep->me_value = value;
		if (index < 0) 
			mp->ma_otablep[mp->ma_used] = ep;
		else {
			epp = mp->ma_otablep;
			epp += index;
			/* make space */
			memmove(epp + 1, epp, (mp->ma_used - index) * sizeof(PyOrderedDictEntry *));
			*epp = ep;
		}
		mp->ma_used++;
	}
	return 0;
}


/*
Internal routine used by dictresize() to insert an item which is
known to be absent from the dict.  This routine also assumes that
the dict contains no deleted entries.  Besides the performance benefit,
using insertdict() in dictresize() is dangerous (SF bug #1456209).
Note that no refcounts are changed by this routine; if needed, the caller
is responsible for incref'ing `key` and `value`.
*/
static void
insertdict_clean(register PyOrderedDictObject *mp, PyObject *key, long hash,
		 PyObject *value)
{
	register size_t i;
	register size_t perturb;
	register size_t mask = (size_t)mp->ma_mask;
	PyOrderedDictEntry *ep0 = mp->ma_table;
	register PyOrderedDictEntry *ep;

	i = hash & mask;
	ep = &ep0[i];
	for (perturb = hash; ep->me_key != NULL; perturb >>= PERTURB_SHIFT) {
		i = (i << 2) + i + perturb + 1;
		ep = &ep0[i & mask];
	}
	assert(ep->me_value == NULL);
	mp->ma_fill++;
	ep->me_key = key;
	ep->me_hash = (Py_ssize_t)hash;
	ep->me_value = value;
	mp->ma_otablep[mp->ma_used] = ep;
	mp->ma_used++;
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
*/
static int
dictresize(PyOrderedDictObject *mp, Py_ssize_t minused)
{
	Py_ssize_t newsize;
	PyOrderedDictEntry *oldtable, *newtable, *ep, **epp;
	PyOrderedDictEntry **oldotablep, **newotablep;
	register Py_ssize_t i, j;
	int is_oldtable_malloced;
	PyOrderedDictEntry small_copy[PyOrderedDict_MINSIZE];
	PyOrderedDictEntry *small_ocopyp[PyOrderedDict_MINSIZE];

	assert(minused >= 0);

	/* Find the smallest table size > minused. */
	for (newsize = PyOrderedDict_MINSIZE;
	     newsize <= minused && newsize > 0;
	     newsize <<= 1)
		;
	if (newsize <= 0) {
		PyErr_NoMemory();
		return -1;
	}

	/* Get space for a new table. */
	oldtable = mp->ma_table;
	oldotablep = mp->ma_otablep;
	assert(oldtable != NULL);
	assert(oldotablep != NULL);
	is_oldtable_malloced = oldtable != mp->ma_smalltable;

	if (newsize == PyOrderedDict_MINSIZE) {
		/* A large table is shrinking, or we can't get any smaller. */
		newtable = mp->ma_smalltable;
		newotablep = mp->ma_smallotablep;
		if (newtable == oldtable) {
			if (mp->ma_fill == mp->ma_used) {
				/* No dummies, so no point doing anything. */
				return 0;
			}
			/* We're not going to resize it, but rebuild the
			   table anyway to purge old dummy entries.
			   Subtle:  This is *necessary* if fill==size,
			   as lookdict needs at least one virgin slot to
			   terminate failing searches.  If fill < size, it's
			   merely desirable, as dummies slow searches. */
			assert(mp->ma_fill > mp->ma_used);
			memcpy(small_copy, oldtable, sizeof(small_copy));
			memcpy(small_ocopyp, oldotablep, sizeof(small_ocopyp));
			oldtable = small_copy;
		}
	}
	else {
		newtable = PyMem_NEW(PyOrderedDictEntry, newsize);
		if (newtable == NULL) {
			PyErr_NoMemory();
			return -1;
		}
		newotablep = PyMem_NEW(PyOrderedDictEntry*, newsize);
		if (newotablep == NULL) {
			PyErr_NoMemory();
			return -1;
		}
	}

	/* Make the dict empty, using the new table. */
	assert(newtable != oldtable);
	assert(newotablep != oldotablep);
	mp->ma_table = newtable;
	mp->ma_otablep = newotablep;
	mp->ma_mask = newsize - 1;
	memset(newtable, 0, sizeof(PyOrderedDictEntry) * newsize);
	memcpy(newotablep, oldotablep, sizeof(PyOrderedDictEntry *) * newsize);
	epp = mp->ma_otablep;
	j = mp->ma_used;
	mp->ma_used = 0;
	i = mp->ma_fill;
	mp->ma_fill = 0;

	/* Copy the data over; this is refcount-neutral for active entries;
	   dummy entries aren't copied over, of course */

	for (epp = mp->ma_otablep; j > 0; epp++, j--) {
		insertdict_clean(mp, (*epp)->me_key, (long)(*epp)->me_hash,
					 (*epp)->me_value);
	}
	for (ep = oldtable; i > 0; ep++) {
		if (ep->me_value != NULL) {	/* active entry */
			--i;
		}
		else if (ep->me_key != NULL) {	/* dummy entry */
			--i;
			assert(ep->me_key == dummy);
			Py_DECREF(ep->me_key);
		}
		/* else key == value == NULL:  nothing to do */
	}

	if (is_oldtable_malloced) {
		PyMem_DEL(oldtable);
		PyMem_DEL(oldotablep);
	}
	return 0;
}

/* Note that, for historical reasons, PyOrderedDict_GetItem() suppresses all errors
 * that may occur (originally dicts supported only string keys, and exceptions
 * weren't possible).  So, while the original intent was that a NULL return
 * meant the key wasn't present, in reality it can mean that, or that an error
 * (suppressed) occurred while computing the key's hash, or that some error
 * (suppressed) occurred when comparing keys in the dict's internal probe
 * sequence.  A nasty example of the latter is when a Python-coded comparison
 * function hits a stack-depth error, which can cause this to return NULL
 * even if the key is present.
 */
PyObject *
PyOrderedDict_GetItem(PyObject *op, PyObject *key)
{
	long hash;
	PyOrderedDictObject *mp = (PyOrderedDictObject *)op;
	PyOrderedDictEntry *ep;
	PyThreadState *tstate;

	if (!PyOrderedDict_Check(op))
		return NULL;
	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1)
	{
		hash = PyObject_Hash(key);
		if (hash == -1) {
			PyErr_Clear();
			return NULL;
		}
	}

	/* We can arrive here with a NULL tstate during initialization:
	   try running "python -Wi" for an example related to string
	   interning.  Let's just hope that no exception occurs then... */
	tstate = _PyThreadState_Current;
	if (tstate != NULL && tstate->curexc_type != NULL) {
		/* preserve the existing exception */
		PyObject *err_type, *err_value, *err_tb;
		PyErr_Fetch(&err_type, &err_value, &err_tb);
		ep = (mp->ma_lookup)(mp, key, hash);
		/* ignore errors */
		PyErr_Restore(err_type, err_value, err_tb);
		if (ep == NULL)
			return NULL;
	}
	else {
		ep = (mp->ma_lookup)(mp, key, hash);
		if (ep == NULL) {
			PyErr_Clear();
			return NULL;
		}
	}
	return ep->me_value;
}

/* CAUTION: PyOrderedDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyOrderedDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
PyOrderedDict_SetItem(register PyObject *op, PyObject *key, PyObject *value)
{
	register PyOrderedDictObject *mp;
	register long hash;
	register Py_ssize_t n_used;

	if (!PyOrderedDict_Check(op)) {
		PyErr_BadInternalCall();
		return -1;
	}
	assert(key);
	assert(value);
	mp = (PyOrderedDictObject *)op;
	if (PyString_CheckExact(key)) {
		hash = ((PyStringObject *)key)->ob_shash;
		if (hash == -1)
			hash = PyObject_Hash(key);
	}
	else {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}
	assert(mp->ma_fill <= mp->ma_mask);  /* at least one empty slot */
	n_used = mp->ma_used;
	Py_INCREF(value);
	Py_INCREF(key);
	if (insertdict(mp, key, hash, value, -1) != 0)
		return -1;
	/* If we added a key, we can safely resize.  Otherwise just return!
	 * If fill >= 2/3 size, adjust size.  Normally, this doubles or
	 * quaduples the size, but it's also possible for the dict to shrink
	 * (if ma_fill is much larger than ma_used, meaning a lot of dict
	 * keys have been * deleted).
	 *
	 * Quadrupling the size improves average dictionary sparseness
	 * (reducing collisions) at the cost of some memory and iteration
	 * speed (which loops over every possible entry).  It also halves
	 * the number of expensive resize operations in a growing dictionary.
	 *
	 * Very large dictionaries (over 50K items) use doubling instead.
	 * This may help applications with severe memory constraints.
	 */
	if (!(mp->ma_used > n_used && mp->ma_fill*3 >= (mp->ma_mask+1)*2))
		return 0;
	return dictresize(mp, (mp->ma_used > 50000 ? 2 : 4) * mp->ma_used);
}

int
PyOrderedDict_InsertItem(register PyOrderedDictObject *mp, Py_ssize_t index, 
						 PyObject *key, PyObject *value)
{
	register long hash;
	register Py_ssize_t n_used;

	if (!PyOrderedDict_Check(mp)) {
		PyErr_BadInternalCall();
		return -1;
	}
	assert(key);
	assert(value);
	if (index < 0)
		index += mp->ma_used;
	/* test to see if index is in range */
	if (index < 0 || index >= mp->ma_used) {
		PyErr_SetString(PyExc_IndexError,
				"insert(): index out of range");
		return -1;
	}
	if (PyString_CheckExact(key)) {
		hash = ((PyStringObject *)key)->ob_shash;
		if (hash == -1)
			hash = PyObject_Hash(key);
	}
	else {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}
	assert(mp->ma_fill <= mp->ma_mask);  /* at least one empty slot */
	n_used = mp->ma_used;
	Py_INCREF(value);
	Py_INCREF(key);
	if (insertdict(mp, key, hash, value, index) != 0)
		return -1;
	/* If we added a key, we can safely resize.  Otherwise just return!
	 * If fill >= 2/3 size, adjust size.  Normally, this doubles or
	 * quaduples the size, but it's also possible for the dict to shrink
	 * (if ma_fill is much larger than ma_used, meaning a lot of dict
	 * keys have been * deleted).
	 *
	 * Quadrupling the size improves average dictionary sparseness
	 * (reducing collisions) at the cost of some memory and iteration
	 * speed (which loops over every possible entry).  It also halves
	 * the number of expensive resize operations in a growing dictionary.
	 *
	 * Very large dictionaries (over 50K items) use doubling instead.
	 * This may help applications with severe memory constraints.
	 */
	if (!(mp->ma_used > n_used && mp->ma_fill*3 >= (mp->ma_mask+1)*2))
		return 0;
	return dictresize(mp, (mp->ma_used > 50000 ? 2 : 4) * mp->ma_used);
}


static int
del_inorder(PyOrderedDictObject *op, PyOrderedDictEntry* ep)
{
	register Py_ssize_t count = op->ma_used;
	PyOrderedDictEntry **tmp = op->ma_otablep;
	while (count--) {
		if (*tmp == ep) {
			memmove(tmp, tmp+1, count * sizeof(PyOrderedDictEntry *));
			return 1;
		}
		tmp++;
	}
	return 0; /* not found */
}

int
PyOrderedDict_DelItem(PyObject *op, PyObject *key)
{
	register PyOrderedDictObject *mp;
	register long hash;
	register PyOrderedDictEntry *ep;
	PyObject *old_value, *old_key;

	if (!PyOrderedDict_Check(op)) {
		PyErr_BadInternalCall();
		return -1;
	}
	assert(key);
	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}
	mp = (PyOrderedDictObject *)op;
	ep = (mp->ma_lookup)(mp, key, hash);
	/* at this point we have to move all the entries beyond the one found
	back on space (this could be optimised by deferring)  */
	del_inorder(mp, ep);
	if (ep == NULL)
		return -1;
	if (ep->me_value == NULL) {
		set_key_error(key);
		return -1;
	}
	old_key = ep->me_key;
	assert(ep->me_key);
	Py_INCREF(dummy);
	ep->me_key = dummy;
	old_value = ep->me_value;
	ep->me_value = NULL;
	mp->ma_used--;
	Py_DECREF(old_value);
	Py_DECREF(old_key);
	return 0;
}

void
PyOrderedDict_Clear(PyObject *op)
{
	PyOrderedDictObject *mp;
	PyOrderedDictEntry *ep, *table, **otablep;
	int table_is_malloced;
	Py_ssize_t fill;
	PyOrderedDictEntry small_copy[PyOrderedDict_MINSIZE];
#ifdef Py_DEBUG
	Py_ssize_t i, n;
#endif

	if (!PyOrderedDict_Check(op))
		return;
	mp = (PyOrderedDictObject *)op;
#ifdef Py_DEBUG
	n = mp->ma_mask + 1;
	i = 0;
#endif

	table = mp->ma_table;
	otablep = mp->ma_otablep;
	assert(table != NULL);
	assert(otablep != NULL);
	table_is_malloced = table != mp->ma_smalltable;

	/* This is delicate.  During the process of clearing the dict,
	 * decrefs can cause the dict to mutate.  To avoid fatal confusion
	 * (voice of experience), we have to make the dict empty before
	 * clearing the slots, and never refer to anything via mp->xxx while
	 * clearing.
	 */
	fill = mp->ma_fill;
	if (table_is_malloced)
		EMPTY_TO_MINSIZE(mp);

	else if (fill > 0) {
		/* It's a small table with something that needs to be cleared.
		 * Afraid the only safe way is to copy the dict entries into
		 * another small table first.
		 */
		memcpy(small_copy, table, sizeof(small_copy));
		table = small_copy;
		EMPTY_TO_MINSIZE(mp);
	}
	/* else it's a small table that's already empty */

	/* Now we can finally clear things.  If C had refcounts, we could
	 * assert that the refcount on table is 1 now, i.e. that this function
	 * has unique access to it, so decref side-effects can't alter it.
	 */
	for (ep = table; fill > 0; ++ep) {
#ifdef Py_DEBUG
		assert(i < n);
		++i;
#endif
		if (ep->me_key) {
			--fill;
			Py_DECREF(ep->me_key);
			Py_XDECREF(ep->me_value);
		}
#ifdef Py_DEBUG
		else
			assert(ep->me_value == NULL);
#endif
	}

	if (table_is_malloced) {
		PyMem_DEL(table);
		PyMem_DEL(otablep);
	}
}

/*
 * Iterate over a dict.  Use like so:
 *
 *     Py_ssize_t i;
 *     PyObject *key, *value;
 *     i = 0;   # important!  i should not otherwise be changed by you
 *     while (PyOrderedDict_Next(yourdict, &i, &key, &value)) {
 *              Refer to borrowed references in key and value.
 *     }
 *
 * CAUTION:  In general, it isn't safe to use PyOrderedDict_Next in a loop that
 * mutates the dict.  One exception:  it is safe if the loop merely changes
 * the values associated with the keys (but doesn't insert new keys or
 * delete keys), via PyOrderedDict_SetItem().
 */
int
PyOrderedDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
	register Py_ssize_t i;
	register PyOrderedDictEntry **epp;

	if (!PyOrderedDict_Check(op))
		return 0;
	i = *ppos;
	if (i < 0)
		return 0;
	if (i >= ((PyOrderedDictObject *)op)->ma_used)
		return 0;
	*ppos = i+1;
	epp = ((PyOrderedDictObject *)op)->ma_otablep;
	if (pkey)
		*pkey = epp[i]->me_key;
	if (pvalue)
		*pvalue = epp[i]->me_value;
	return 1;
}

/* Internal version of PyOrderedDict_Next that returns a hash value in addition to the key and value.*/
int
_PyOrderedDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue, long *phash)
{
	register Py_ssize_t i;
	register Py_ssize_t mask;
	register PyOrderedDictEntry *ep;

	if (!PyOrderedDict_Check(op))
		return 0;
	i = *ppos;
	if (i < 0)
		return 0;
	ep = ((PyOrderedDictObject *)op)->ma_table;
	mask = ((PyOrderedDictObject *)op)->ma_mask;
	while (i <= mask && ep[i].me_value == NULL)
		i++;
	*ppos = i+1;
	if (i > mask)
		return 0;
        *phash = (long)(ep[i].me_hash);
	if (pkey)
		*pkey = ep[i].me_key;
	if (pvalue)
		*pvalue = ep[i].me_value;
	return 1;
}

/* Methods */

static void
dict_dealloc(register PyOrderedDictObject *mp)
{
	register PyOrderedDictEntry *ep;
	Py_ssize_t fill = mp->ma_fill;
 	PyObject_GC_UnTrack(mp);
	Py_TRASHCAN_SAFE_BEGIN(mp)
	for (ep = mp->ma_table; fill > 0; ep++) {
		if (ep->me_key) {
			--fill;
			Py_DECREF(ep->me_key);
			Py_XDECREF(ep->me_value);
		}
	}
	if (mp->ma_table != mp->ma_smalltable) {
		PyMem_DEL(mp->ma_table);
		PyMem_DEL(mp->ma_otablep);
        }
	if (num_free_dicts < MAXFREEDICTS && Py_Type(mp) == &PyOrderedDict_Type)
		free_dicts[num_free_dicts++] = mp;
	else
		Py_Type(mp)->tp_free((PyObject *)mp);
	Py_TRASHCAN_SAFE_END(mp)
}

static int
dict_print(register PyOrderedDictObject *mp, register FILE *fp, register int flags)
{
	register Py_ssize_t i;
	register Py_ssize_t any;
	int status;
	PyOrderedDictEntry **epp;

	status = Py_ReprEnter((PyObject*)mp);
	if (status != 0) {
		if (status < 0)
			return status;
		fprintf(fp, "{...}");
		return 0;
	}

	fprintf(fp, "ordereddict([");
	any = 0;
	epp = mp->ma_otablep;
	for (i = 0; i < mp->ma_used; i++) {
		PyObject *pvalue = (*epp)->me_value;
		/* Prevent PyObject_Repr from deleting value during
		   key format */
		Py_INCREF(pvalue);
		if (any++ > 0)
			fprintf(fp, ", ");
		fprintf(fp, "(");
		if (PyObject_Print((PyObject *)((*epp)->me_key), fp, 0)!=0) {
			Py_DECREF(pvalue);
			Py_ReprLeave((PyObject*)mp);
			return -1;
		}
		fprintf(fp, ", ");
		if (PyObject_Print(pvalue, fp, 0) != 0) {
			Py_DECREF(pvalue);
			Py_ReprLeave((PyObject*)mp);
			return -1;
		}
		Py_DECREF(pvalue);
		fprintf(fp, ")");
		epp++;
	}
	fprintf(fp, "])");
	Py_ReprLeave((PyObject*)mp);
	return 0;
}

static PyObject *
dict_repr(PyOrderedDictObject *mp)
{
	Py_ssize_t i;
	PyObject *s, *temp, *comma = NULL, *rightpar = NULL;
	PyObject *pieces = NULL, *result = NULL;
	PyObject *key, *value;

	i = Py_ReprEnter((PyObject *)mp);
	if (i != 0) {
		return i > 0 ? PyString_FromString("{...}") : NULL;
	}

	if (mp->ma_used == 0) {
		result = PyString_FromString("ordereddict([])");
		goto Done;
	}

	pieces = PyList_New(0);
	if (pieces == NULL)
		goto Done;

	comma = PyString_FromString(", ");
	if (comma == NULL)
		goto Done;
	rightpar = PyString_FromString(")");
	if (rightpar == NULL)
		goto Done;

	/* Do repr() on each key+value pair, and insert ": " between them.
	   Note that repr may mutate the dict. */
	i = 0;
	while (PyOrderedDict_Next((PyObject *)mp, &i, &key, &value)) {
		int status;
		/* Prevent repr from deleting value during key format. */
		Py_INCREF(value);
		s = PyString_FromString("(");
		PyString_ConcatAndDel(&s, PyObject_Repr(key));
		PyString_Concat(&s, comma);
		PyString_ConcatAndDel(&s, PyObject_Repr(value));
		Py_DECREF(value);
		PyString_Concat(&s, rightpar);
		if (s == NULL)
			goto Done;
		status = PyList_Append(pieces, s);
		Py_DECREF(s);  /* append created a new ref */
		if (status < 0)
			goto Done;
	}

	/* Add "[]" decorations to the first and last items. */
	assert(PyList_GET_SIZE(pieces) > 0);
	s = PyString_FromString("ordereddict([");
	if (s == NULL)
		goto Done;
	temp = PyList_GET_ITEM(pieces, 0);
	PyString_ConcatAndDel(&s, temp);
	PyList_SET_ITEM(pieces, 0, s);
	if (s == NULL)
		goto Done;

	s = PyString_FromString("])");
	if (s == NULL)
		goto Done;
	temp = PyList_GET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1);
	PyString_ConcatAndDel(&temp, s);
	PyList_SET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1, temp);
	if (temp == NULL)
		goto Done;

	/* Paste them all together with ", " between. */
	result = _PyString_Join(comma, pieces);

Done:
	Py_XDECREF(pieces);
	Py_XDECREF(comma);
	Py_XDECREF(rightpar);
	Py_ReprLeave((PyObject *)mp);
	return result;
}

static Py_ssize_t
dict_length(PyOrderedDictObject *mp)
{
	return mp->ma_used;
}

static PyObject *
dict_subscript(PyOrderedDictObject *mp, register PyObject *key)
{
	PyObject *v;
	long hash;
	PyOrderedDictEntry *ep;
	assert(mp->ma_table != NULL);
	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;
	v = ep->me_value;
	if (v == NULL) {
		if (!PyOrderedDict_CheckExact(mp)) {
			/* Look up __missing__ method if we're a subclass. */
		    	PyObject *missing;
			static PyObject *missing_str = NULL;
			if (missing_str == NULL)
				missing_str =
				  PyString_InternFromString("__missing__");
			missing = _PyType_Lookup(Py_Type(mp), missing_str);
			if (missing != NULL)
				return PyObject_CallFunctionObjArgs(missing,
					(PyObject *)mp, key, NULL);
		}
		set_key_error(key);
		return NULL;
	}
	else
		Py_INCREF(v);
	return v;
}

static int
dict_ass_sub(PyOrderedDictObject *mp, PyObject *v, PyObject *w)
{
	if (w == NULL)
		return PyOrderedDict_DelItem((PyObject *)mp, v);
	else
		return PyOrderedDict_SetItem((PyObject *)mp, v, w);
}

static PyMappingMethods dict_as_mapping = {
	(lenfunc)dict_length, /*mp_length*/
	(binaryfunc)dict_subscript, /*mp_subscript*/
	(objobjargproc)dict_ass_sub, /*mp_ass_subscript*/
};

static PyObject *
dict_keys(register PyOrderedDictObject *mp)
{
	register PyObject *v;
	register Py_ssize_t i;
	PyOrderedDictEntry **epp;
	Py_ssize_t n;

  again:
	n = mp->ma_used;
	v = PyList_New(n);
	if (v == NULL)
		return NULL;
	if (n != mp->ma_used) {
		/* Durnit.  The allocations caused the dict to resize.
		 * Just start over, this shouldn't normally happen.
		 */
		Py_DECREF(v);
		goto again;
	}
	epp = mp->ma_otablep;
	for (i = 0; i < n; i++) {
		PyObject *key = (*epp)->me_key;
		Py_INCREF(key);
		PyList_SET_ITEM(v, i, key);
		epp++;
	}
	return v;
}

static PyObject *
dict_values(register PyOrderedDictObject *mp)
{
	register PyObject *v;
	register Py_ssize_t i;
	PyOrderedDictEntry **epp;
	Py_ssize_t n;

  again:
	n = mp->ma_used;
	v = PyList_New(n);
	if (v == NULL)
		return NULL;
	if (n != mp->ma_used) {
		/* Durnit.  The allocations caused the dict to resize.
		 * Just start over, this shouldn't normally happen.
		 */
		Py_DECREF(v);
		goto again;
	}
	epp = mp->ma_otablep;
	for (i = 0; i < n; i++) {
		PyObject *value = (*epp)->me_value;
		Py_INCREF(value);
		PyList_SET_ITEM(v, i, value);
		epp++;
	}
	return v;
}

static PyObject *
dict_items(register PyOrderedDictObject *mp)
{
	register PyObject *v;
	register Py_ssize_t i, n;
	PyObject *item, *key, *value;
	PyOrderedDictEntry **epp;

	/* Preallocate the list of tuples, to avoid allocations during
	 * the loop over the items, which could trigger GC, which
	 * could resize the dict. :-(
	 */
  again:
	n = mp->ma_used;
	v = PyList_New(n);
	if (v == NULL)
		return NULL;
	for (i = 0; i < n; i++) {
		item = PyTuple_New(2);
		if (item == NULL) {
			Py_DECREF(v);
			return NULL;
		}
		PyList_SET_ITEM(v, i, item);
	}
	if (n != mp->ma_used) {
		/* Durnit.  The allocations caused the dict to resize.
		 * Just start over, this shouldn't normally happen.
		 */
		Py_DECREF(v);
		goto again;
	}
	/* Nothing we do below makes any function calls. */
	epp = mp->ma_otablep;
	for (i = 0; i < n; i++) {
		key = (*epp)->me_key;
		value = (*epp)->me_value;
		item = PyList_GET_ITEM(v, i);
		Py_INCREF(key);
		PyTuple_SET_ITEM(item, 0, key);
		Py_INCREF(value);
		PyTuple_SET_ITEM(item, 1, value);
		epp++;
	}
	return v;
}

static PyObject *
dict_fromkeys(PyObject *cls, PyObject *args)
{
	PyObject *seq;
	PyObject *value = Py_None;
	PyObject *it;	/* iter(seq) */
	PyObject *key;
	PyObject *d;
	int status;

	if (!PyArg_UnpackTuple(args, "fromkeys", 1, 2, &seq, &value))
		return NULL;

	d = PyObject_CallObject(cls, NULL);
	if (d == NULL)
		return NULL;


#if PY_VERSION_HEX >= 0x02050000
	if (PyOrderedDict_CheckExact(d) && PyAnySet_CheckExact(seq)) {
		PyOrderedDictObject *mp = (PyOrderedDictObject *)d;
		Py_ssize_t pos = 0;
		PyObject *key;
		long hash;

		if (dictresize(mp, PySet_GET_SIZE(seq)))
			return NULL;

		while (_PySet_NextEntry(seq, &pos, &key, &hash)) {
			Py_INCREF(key);
			Py_INCREF(value);
			if (insertdict(mp, key, hash, value, -1))
				return NULL;
		}
		return d;
	}
#else
        d = PyObject_CallObject(cls, NULL);
        if (d == NULL)
                return NULL;
#endif

	it = PyObject_GetIter(seq);
	if (it == NULL){
		Py_DECREF(d);
		return NULL;
	}

	for (;;) {
		key = PyIter_Next(it);
		if (key == NULL) {
			if (PyErr_Occurred())
				goto Fail;
			break;
		}
		status = PyObject_SetItem(d, key, value);
		Py_DECREF(key);
		if (status < 0)
			goto Fail;
	}

	Py_DECREF(it);
	return d;

Fail:
	Py_DECREF(it);
	Py_DECREF(d);
	return NULL;
}

static int
dict_update_common(PyObject *self, PyObject *args, PyObject *kwds, char *methname)
{
	PyObject *arg = NULL;
	int result = 0;

	if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg))
		result = -1;
	else if (arg != NULL) {
		if (PyObject_HasAttrString(arg, "keys"))
			result = PyOrderedDict_Merge(self, arg, 1);
		else
			result = PyOrderedDict_MergeFromSeq2(self, arg, 1);
	}
	if (result == 0 && kwds != NULL)
		result = PyOrderedDict_Merge(self, kwds, 1);
	return result;
}

static PyObject *
dict_update(PyObject *self, PyObject *args, PyObject *kwds)
{
	if (dict_update_common(self, args, kwds, "update") != -1)
		Py_RETURN_NONE;
	return NULL;
}

/* Update unconditionally replaces existing items.
   Merge has a 3rd argument 'override'; if set, it acts like Update,
   otherwise it leaves existing items unchanged.

   PyOrderedDict_{Update,Merge} update/merge from a mapping object.

   PyOrderedDict_MergeFromSeq2 updates/merges from any iterable object
   producing iterable objects of length 2.
*/

int
PyOrderedDict_MergeFromSeq2(PyObject *d, PyObject *seq2, int override)
{
	PyObject *it;	/* iter(seq2) */
	Py_ssize_t i;	/* index into seq2 of current element */
	PyObject *item;	/* seq2[i] */
	PyObject *fast;	/* item as a 2-tuple or 2-list */

	assert(d != NULL);
	assert(PyOrderedDict_Check(d));
	assert(seq2 != NULL);

	it = PyObject_GetIter(seq2);
	if (it == NULL)
		return -1;

	for (i = 0; ; ++i) {
		PyObject *key, *value;
		Py_ssize_t n;

		fast = NULL;
		item = PyIter_Next(it);
		if (item == NULL) {
			if (PyErr_Occurred())
				goto Fail;
			break;
		}

		/* Convert item to sequence, and verify length 2. */
		fast = PySequence_Fast(item, "");
		if (fast == NULL) {
			if (PyErr_ExceptionMatches(PyExc_TypeError))
				PyErr_Format(PyExc_TypeError,
					"cannot convert dictionary update "
					"sequence element #%zd to a sequence",
					i);
			goto Fail;
		}
		n = PySequence_Fast_GET_SIZE(fast);
		if (n != 2) {
			PyErr_Format(PyExc_ValueError,
				     "dictionary update sequence element #%zd "
				     "has length %zd; 2 is required",
				     i, n);
			goto Fail;
		}

		/* Update/merge with this (key, value) pair. */
		key = PySequence_Fast_GET_ITEM(fast, 0);
		value = PySequence_Fast_GET_ITEM(fast, 1);
		if (override || PyOrderedDict_GetItem(d, key) == NULL) {
			int status = PyOrderedDict_SetItem(d, key, value);
			if (status < 0)
				goto Fail;
		}
		Py_DECREF(fast);
		Py_DECREF(item);
	}

	i = 0;
	goto Return;
Fail:
	Py_XDECREF(item);
	Py_XDECREF(fast);
	i = -1;
Return:
	Py_DECREF(it);
	return Py_SAFE_DOWNCAST(i, Py_ssize_t, int);
}

int
PyOrderedDict_Update(PyObject *a, PyObject *b)
{
	return PyOrderedDict_Merge(a, b, 1);
}

int
PyOrderedDict_Merge(PyObject *a, PyObject *b, int override)
{
	register PyOrderedDictObject *mp, *other;
	register Py_ssize_t i;
	PyOrderedDictEntry *entry, **epp;

	/* We accept for the argument either a concrete dictionary object,
	 * or an abstract "mapping" object.  For the former, we can do
	 * things quite efficiently.  For the latter, we only require that
	 * PyMapping_Keys() and PyObject_GetItem() be supported.
	 */
	if (a == NULL || !PyOrderedDict_Check(a) || b == NULL) {
		PyErr_BadInternalCall();
		return -1;
	}
	mp = (PyOrderedDictObject*)a;
	if (PyOrderedDict_CheckExact(b)) {
		other = (PyOrderedDictObject*)b;
		if (other == mp || other->ma_used == 0)
			/* a.update(a) or a.update({}); nothing to do */
			return 0;
		if (mp->ma_used == 0)
			/* Since the target dict is empty, PyOrderedDict_GetItem()
			 * always returns NULL.  Setting override to 1
			 * skips the unnecessary test.
			 */
			override = 1;
		/* Do one big resize at the start, rather than
		 * incrementally resizing as we insert new items.  Expect
		 * that there will be no (or few) overlapping keys.
		 */
		if ((mp->ma_fill + other->ma_used)*3 >= (mp->ma_mask+1)*2) {
		   if (dictresize(mp, (mp->ma_used + other->ma_used)*2) != 0)
			   return -1;
		}
		epp = other->ma_otablep;
		for (i = 0; i < other->ma_used; i++) {
			entry = *epp++;
			if (entry->me_value != NULL &&
			    (override ||
			     PyOrderedDict_GetItem(a, entry->me_key) == NULL)) {
				Py_INCREF(entry->me_key);
				Py_INCREF(entry->me_value);
				if (insertdict(mp, entry->me_key,
					       (long)entry->me_hash,
					       entry->me_value, -1) != 0)
					return -1;
			}
		}
	}
	else {
		PyErr_SetString(PyExc_TypeError,
				"source has undefined order");
		return -1;
	}
	return 0;
}

static PyObject *
dict_copy(register PyOrderedDictObject *mp)
{
	return PyOrderedDict_Copy((PyObject*)mp);
}

PyObject *
PyOrderedDict_Copy(PyObject *o)
{
	PyObject *copy;

	if (o == NULL || !PyOrderedDict_Check(o)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	copy = PyOrderedDict_New();
	if (copy == NULL)
		return NULL;
	if (PyOrderedDict_Merge(copy, o, 1) == 0)
		return copy;
	Py_DECREF(copy);
	return NULL;
}

Py_ssize_t
PyOrderedDict_Size(PyObject *mp)
{
	if (mp == NULL || !PyOrderedDict_Check(mp)) {
		PyErr_BadInternalCall();
		return -1;
	}
	return ((PyOrderedDictObject *)mp)->ma_used;
}

PyObject *
PyOrderedDict_Keys(PyObject *mp)
{
	if (mp == NULL || !PyOrderedDict_Check(mp)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return dict_keys((PyOrderedDictObject *)mp);
}

PyObject *
PyOrderedDict_Values(PyObject *mp)
{
	if (mp == NULL || !PyOrderedDict_Check(mp)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return dict_values((PyOrderedDictObject *)mp);
}

PyObject *
PyOrderedDict_Items(PyObject *mp)
{
	if (mp == NULL || !PyOrderedDict_Check(mp)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return dict_items((PyOrderedDictObject *)mp);
}

/* Subroutine which returns the smallest key in a for which b's value
   is different or absent.  The value is returned too, through the
   pval argument.  Both are NULL if no key in a is found for which b's status
   differs.  The refcounts on (and only on) non-NULL *pval and function return
   values must be decremented by the caller (characterize() increments them
   to ensure that mutating comparison and PyOrderedDict_GetItem calls can't delete
   them before the caller is done looking at them). */

static PyObject *
characterize(PyOrderedDictObject *a, PyOrderedDictObject *b, PyObject **pval)
{
	PyObject *akey = NULL; /* smallest key in a s.t. a[akey] != b[akey] */
	PyObject *aval = NULL; /* a[akey] */
	Py_ssize_t i;
	int cmp;

	for (i = 0; i <= a->ma_mask; i++) {
		PyObject *thiskey, *thisaval, *thisbval;
		if (a->ma_table[i].me_value == NULL)
			continue;
		thiskey = a->ma_table[i].me_key;
		Py_INCREF(thiskey);  /* keep alive across compares */
		if (akey != NULL) {
			cmp = PyObject_RichCompareBool(akey, thiskey, Py_LT);
			if (cmp < 0) {
				Py_DECREF(thiskey);
				goto Fail;
			}
			if (cmp > 0 ||
			    i > a->ma_mask ||
			    a->ma_table[i].me_value == NULL)
			{
				/* Not the *smallest* a key; or maybe it is
				 * but the compare shrunk the dict so we can't
				 * find its associated value anymore; or
				 * maybe it is but the compare deleted the
				 * a[thiskey] entry.
				 */
				Py_DECREF(thiskey);
				continue;
			}
		}

		/* Compare a[thiskey] to b[thiskey]; cmp <- true iff equal. */
		thisaval = a->ma_table[i].me_value;
		assert(thisaval);
		Py_INCREF(thisaval);   /* keep alive */
		thisbval = PyOrderedDict_GetItem((PyObject *)b, thiskey);
		if (thisbval == NULL)
			cmp = 0;
		else {
			/* both dicts have thiskey:  same values? */
			cmp = PyObject_RichCompareBool(
						thisaval, thisbval, Py_EQ);
			if (cmp < 0) {
		    		Py_DECREF(thiskey);
		    		Py_DECREF(thisaval);
		    		goto Fail;
			}
		}
		if (cmp == 0) {
			/* New winner. */
			Py_XDECREF(akey);
			Py_XDECREF(aval);
			akey = thiskey;
			aval = thisaval;
		}
		else {
			Py_DECREF(thiskey);
			Py_DECREF(thisaval);
		}
	}
	*pval = aval;
	return akey;

Fail:
	Py_XDECREF(akey);
	Py_XDECREF(aval);
	*pval = NULL;
	return NULL;
}

static int
dict_compare(PyOrderedDictObject *a, PyOrderedDictObject *b)
{
	PyObject *adiff, *bdiff, *aval, *bval;
	int res;

	/* Compare lengths first */
	if (a->ma_used < b->ma_used)
		return -1;	/* a is shorter */
	else if (a->ma_used > b->ma_used)
		return 1;	/* b is shorter */

	/* Same length -- check all keys */
	bdiff = bval = NULL;
	adiff = characterize(a, b, &aval);
	if (adiff == NULL) {
		assert(!aval);
		/* Either an error, or a is a subset with the same length so
		 * must be equal.
		 */
		res = PyErr_Occurred() ? -1 : 0;
		goto Finished;
	}
	bdiff = characterize(b, a, &bval);
	if (bdiff == NULL && PyErr_Occurred()) {
		assert(!bval);
		res = -1;
		goto Finished;
	}
	res = 0;
	if (bdiff) {
		/* bdiff == NULL "should be" impossible now, but perhaps
		 * the last comparison done by the characterize() on a had
		 * the side effect of making the dicts equal!
		 */
		res = PyObject_Compare(adiff, bdiff);
	}
	if (res == 0 && bval != NULL)
		res = PyObject_Compare(aval, bval);

Finished:
	Py_XDECREF(adiff);
	Py_XDECREF(bdiff);
	Py_XDECREF(aval);
	Py_XDECREF(bval);
	return res;
}

/* Return 1 if dicts equal, 0 if not, -1 if error.
 * Gets out as soon as any difference is detected.
 * Uses only Py_EQ comparison.
 */
static int
dict_equal(PyOrderedDictObject *a, PyOrderedDictObject *b)
{
	Py_ssize_t i;
	PyOrderedDictEntry **app, **bpp;

	if (a->ma_used != b->ma_used)
		/* can't be equal if # of entries differ */
		return 0;

	/* Same # of entries -- check all of 'em.  Exit early on any diff. */
	
	for (i = 0, app = a->ma_otablep, bpp = b->ma_otablep; i < a->ma_used;
		i++, app++, bpp++) {
		int cmp;
		PyObject *aval = (*app)->me_value;
		PyObject *bval = (*bpp)->me_value;
		PyObject *akey = (*app)->me_key;
		PyObject *bkey = (*bpp)->me_key;
		/* temporarily bump aval's refcount to ensure it stays
		   alive until we're done with it */
		Py_INCREF(aval);
		Py_INCREF(bval);
		/* ditto for key */
		Py_INCREF(akey);
		Py_INCREF(bkey);
		cmp = PyObject_RichCompareBool(akey, bkey, Py_EQ);
		if (cmp > 0) /* keys compare ok, now do values */
			cmp = PyObject_RichCompareBool(aval, bval, Py_EQ);
		Py_DECREF(bkey);
		Py_DECREF(akey);
		Py_DECREF(bval);
		Py_DECREF(aval);
		if (cmp <= 0)  /* error or not equal */
			return cmp;
	}
	return 1;
 }

static PyObject *
dict_richcompare(PyObject *v, PyObject *w, int op)
{
	int cmp;
	PyObject *res;

	if (!PyOrderedDict_Check(v) || !PyOrderedDict_Check(w)) {
		res = Py_NotImplemented;
	}
	else if (op == Py_EQ || op == Py_NE) {
		cmp = dict_equal((PyOrderedDictObject *)v, (PyOrderedDictObject *)w);
		if (cmp < 0)
			return NULL;
		res = (cmp == (op == Py_EQ)) ? Py_True : Py_False;
	}
	else
		res = Py_NotImplemented;
	Py_INCREF(res);
	return res;
 }

static PyObject *
dict_contains(register PyOrderedDictObject *mp, PyObject *key)
{
	long hash;
	PyOrderedDictEntry *ep;

	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;
	return PyBool_FromLong(ep->me_value != NULL);
}


static PyObject *
dict_has_key(register PyOrderedDictObject *mp, PyObject *key)
{
	long hash;
	PyOrderedDictEntry *ep;

#if PY_VERSION_HEX >= 0x02060000
	if (Py_Py3kWarningFlag &&
	    PyErr_Warn(PyExc_DeprecationWarning, 
		       "dict.has_key() not supported in 3.x") < 0)
		return NULL;
	return dict_contains(mp, key);
#else
	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;
	return PyBool_FromLong(ep->me_value != NULL);
#endif
}

static PyObject *
dict_get(register PyOrderedDictObject *mp, PyObject *args)
{
	PyObject *key;
	PyObject *failobj = Py_None;
	PyObject *val = NULL;
	long hash;
	PyOrderedDictEntry *ep;

	if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &failobj))
		return NULL;

	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;
	val = ep->me_value;
	if (val == NULL)
		val = failobj;
	Py_INCREF(val);
	return val;
}


static PyObject *
dict_setdefault(register PyOrderedDictObject *mp, PyObject *args)
{
	PyObject *key;
	PyObject *failobj = Py_None;
	PyObject *val = NULL;
	long hash;
	PyOrderedDictEntry *ep;

	if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &failobj))
		return NULL;

	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;
	val = ep->me_value;
	if (val == NULL) {
		val = failobj;
		if (PyOrderedDict_SetItem((PyObject*)mp, key, failobj))
			val = NULL;
	}
	Py_XINCREF(val);
	return val;
}


static PyObject *
dict_clear(register PyOrderedDictObject *mp)
{
	PyOrderedDict_Clear((PyObject *)mp);
	Py_RETURN_NONE;
}

static PyObject *
dict_pop(PyOrderedDictObject *mp, PyObject *args)
{
	long hash;
	PyOrderedDictEntry *ep;
	PyObject *old_value, *old_key;
	PyObject *key, *deflt = NULL;

	if(!PyArg_UnpackTuple(args, "pop", 1, 2, &key, &deflt))
		return NULL;
	if (mp->ma_used == 0) {
		if (deflt) {
			Py_INCREF(deflt);
			return deflt;
		}
		PyErr_SetString(PyExc_KeyError,
				"pop(): dictionary is empty");
		return NULL;
	}
	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;
	if (ep->me_value == NULL) {
		if (deflt) {
			Py_INCREF(deflt);
			return deflt;
		}
		set_key_error(key);
		return NULL;
	}
	old_key = ep->me_key;
	Py_INCREF(dummy);
	ep->me_key = dummy;
	old_value = ep->me_value;
	ep->me_value = NULL;
	del_inorder(mp, ep);
	mp->ma_used--;
	Py_DECREF(old_key);
	return old_value;
}

static PyObject *
dict_popitem(PyOrderedDictObject *mp, PyObject *args)
{
	Py_ssize_t i = -1, j;
	PyOrderedDictEntry **epp;
	PyObject *res;

	/* Allocate the result tuple before checking the size.  Believe it
	 * or not, this allocation could trigger a garbage collection which
	 * could empty the dict, so if we checked the size first and that
	 * happened, the result would be an infinite loop (searching for an
	 * entry that no longer exists).  Note that the usual popitem()
	 * idiom is "while d: k, v = d.popitem()". so needing to throw the
	 * tuple away if the dict *is* empty isn't a significant
	 * inefficiency -- possible, but unlikely in practice.
	 */
#if PY_VERSION_HEX >= 0x02050000
	if (!PyArg_ParseTuple(args, "|n:popitem", &i))
#else
	if (!PyArg_ParseTuple(args, "|i:popitem", &i))
#endif
                return NULL;
				
	res = PyTuple_New(2);
	if (res == NULL)
		return NULL;
	if (mp->ma_used == 0) {
		Py_DECREF(res);
		PyErr_SetString(PyExc_KeyError,
				"popitem(): dictionary is empty");
		return NULL;
	}
	if (i < 0)
		j = mp->ma_used + i;
	else
		j = i;
	if (j < 0 || j >= mp->ma_used) {
		Py_DECREF(res);
		PyErr_SetString(PyExc_KeyError,
				"popitem(): index out of range");
		return NULL;
	}
	epp = mp->ma_otablep;
	epp += j;
	PyTuple_SET_ITEM(res, 0, (*epp)->me_key);
	PyTuple_SET_ITEM(res, 1, (*epp)->me_value);
	Py_INCREF(dummy);
	(*epp)->me_key = dummy;
	(*epp)->me_value = NULL;
	mp->ma_used--;
	if (i != -1) { /* for default case -1, we don't have to do anything */
		/* ma_used has already been decremented ! */
		memmove(epp, epp+1, (mp->ma_used - j) * sizeof(PyOrderedDictEntry *));
	}
	return res;
}

static int
dict_traverse(PyObject *op, visitproc visit, void *arg)
{
	Py_ssize_t i = 0;
	PyObject *pk;
	PyObject *pv;

	while (PyOrderedDict_Next(op, &i, &pk, &pv)) {
		Py_VISIT(pk);
		Py_VISIT(pv);
	}
	return 0;
}

static int
dict_tp_clear(PyObject *op)
{
	PyOrderedDict_Clear(op);
	return 0;
}


extern PyTypeObject PyOrderedDictIterKey_Type; /* Forward */
extern PyTypeObject PyOrderedDictIterValue_Type; /* Forward */
extern PyTypeObject PyOrderedDictIterItem_Type; /* Forward */
static PyObject *dictiter_new(PyOrderedDictObject *, PyTypeObject *);

static PyObject *
dict_iterkeys(PyOrderedDictObject *dict)
{
	return dictiter_new(dict, &PyOrderedDictIterKey_Type);
}

static PyObject *
dict_itervalues(PyOrderedDictObject *dict)
{
	return dictiter_new(dict, &PyOrderedDictIterValue_Type);
}

static PyObject *
dict_iteritems(PyOrderedDictObject *dict)
{
	return dictiter_new(dict, &PyOrderedDictIterItem_Type);
}

static PyObject *
dict_index(register PyOrderedDictObject *mp, PyObject *key)
{
	long hash;
	PyOrderedDictEntry *ep, **tmp;
	register Py_ssize_t index;

	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	if (ep == NULL)
		return NULL;

	for (index = 0, tmp = mp->ma_otablep; index < mp->ma_used; index++, tmp++) {
		if (*tmp == ep) {
			return PyInt_FromSize_t(index);
		}
	}
	return NULL; /* not found */
}

static PyObject *
dict_insert(PyOrderedDictObject *mp, PyObject *args)
{
	Py_ssize_t i;
	PyObject *key;
	PyObject *val;
	
#if PY_VERSION_HEX >= 0x02050000
    if (!PyArg_ParseTuple(args, "nOO:insert", &i, &key, &val))
#else
    if (!PyArg_ParseTuple(args, "iOO:insert", &i, &key, &val))
#endif
		return NULL;
	if(PyOrderedDict_InsertItem(mp, i, key, val) != 0)
		return NULL;
	Py_RETURN_NONE;
}

static PyObject *
dict_reverse(register PyOrderedDictObject *mp)
{
	PyOrderedDictEntry **epps, **eppe, *tmp;

	epps = mp->ma_otablep;
	eppe = epps + ((mp->ma_used)-1);
	while (epps < eppe) {
		tmp = *epps;
		*epps++ = *eppe;
		*eppe-- = tmp;
	}
	Py_RETURN_NONE;
}


PyDoc_STRVAR(has_key__doc__,
"D.has_key(k) -> True if D has a key k, else False");

PyDoc_STRVAR(contains__doc__,
"D.__contains__(k) -> True if D has a key k, else False");

PyDoc_STRVAR(getitem__doc__, "x.__getitem__(y) <==> x[y]");

PyDoc_STRVAR(get__doc__,
"D.get(k[,d]) -> D[k] if k in D, else d.  d defaults to None.");

PyDoc_STRVAR(setdefault_doc__,
"D.setdefault(k[,d]) -> D.get(k,d), also set D[k]=d if k not in D");

PyDoc_STRVAR(pop__doc__,
"D.pop(k[,d]) -> v, remove specified key and return the corresponding value\n\
If key is not found, d is returned if given, otherwise KeyError is raised");

PyDoc_STRVAR(popitem__doc__,
"D.popitem([index]) -> (k, v), remove and return indexed (key, value) pair as a\n\
2-tuple (default is last); but raise KeyError if D is empty");

PyDoc_STRVAR(keys__doc__,
"D.keys() -> list of D's keys");

PyDoc_STRVAR(items__doc__,
"D.items() -> list of D's (key, value) pairs, as 2-tuples");

PyDoc_STRVAR(values__doc__,
"D.values() -> list of D's values");

PyDoc_STRVAR(update__doc__,
"D.update(E, **F) -> None.  Update D from E and F: for k in E: D[k] = E[k]\n\
(if E has keys else: for (k, v) in E: D[k] = v) then: for k in F: D[k] = F[k]");

PyDoc_STRVAR(fromkeys__doc__,
"dict.fromkeys(S[,v]) -> New dict with keys from S and values equal to v.\n\
v defaults to None.");

PyDoc_STRVAR(clear__doc__,
"D.clear() -> None.  Remove all items from D.");

PyDoc_STRVAR(copy__doc__,
"D.copy() -> a shallow copy of D");

PyDoc_STRVAR(iterkeys__doc__,
"D.iterkeys() -> an iterator over the keys of D");

PyDoc_STRVAR(itervalues__doc__,
"D.itervalues() -> an iterator over the values of D");

PyDoc_STRVAR(iteritems__doc__,
"D.iteritems() -> an iterator over the (key, value) items of D");

PyDoc_STRVAR(index_doc,
"D.index(key) -> return position of key in ordered dict");

PyDoc_STRVAR(insert_doc,
"D.insert(index, key, value) -> add/update (key, value) and insert key at index");

PyDoc_STRVAR(reverse_doc,
"D.reverse() -> reverse the order of the keys of D");

static PyMethodDef mapp_methods[] = {
	{"__contains__",(PyCFunction)dict_contains,   METH_O | METH_COEXIST, 
	 contains__doc__},
	{"__getitem__", (PyCFunction)dict_subscript, METH_O | METH_COEXIST,
	 getitem__doc__},
	{"has_key",	(PyCFunction)dict_has_key,      METH_O,
	 has_key__doc__},
	{"get",         (PyCFunction)dict_get,          METH_VARARGS,
	 get__doc__},
	{"setdefault",  (PyCFunction)dict_setdefault,   METH_VARARGS,
	 setdefault_doc__},
	{"pop",         (PyCFunction)dict_pop,          METH_VARARGS,
	 pop__doc__},
	{"popitem",	(PyCFunction)dict_popitem,	METH_VARARGS,
	 popitem__doc__},
	{"keys",	(PyCFunction)dict_keys,		METH_NOARGS,
	keys__doc__},
	{"items",	(PyCFunction)dict_items,	METH_NOARGS,
	 items__doc__},
	{"values",	(PyCFunction)dict_values,	METH_NOARGS,
	 values__doc__},
	{"update",	(PyCFunction)dict_update,	METH_VARARGS | METH_KEYWORDS,
	 update__doc__},
	{"fromkeys",	(PyCFunction)dict_fromkeys,	METH_VARARGS | METH_CLASS,
	 fromkeys__doc__},
	{"clear",	(PyCFunction)dict_clear,	METH_NOARGS,
	 clear__doc__},
	{"copy",	(PyCFunction)dict_copy,		METH_NOARGS,
	 copy__doc__},
	{"iterkeys",	(PyCFunction)dict_iterkeys,	METH_NOARGS,
	 iterkeys__doc__},
	{"itervalues",	(PyCFunction)dict_itervalues,	METH_NOARGS,
	 itervalues__doc__},
	{"iteritems",	(PyCFunction)dict_iteritems,	METH_NOARGS,
	 iteritems__doc__},
	{"index",       (PyCFunction)dict_index,   METH_O, index_doc},
	{"insert",       (PyCFunction)dict_insert,   METH_VARARGS, insert_doc},
	{"reverse",       (PyCFunction)dict_reverse,   METH_NOARGS, reverse_doc},
	{NULL,		NULL}	/* sentinel */
};

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
int
PyOrderedDict_Contains(PyObject *op, PyObject *key)
{
	long hash;
	PyOrderedDictObject *mp = (PyOrderedDictObject *)op;
	PyOrderedDictEntry *ep;

	if (!PyString_CheckExact(key) ||
	    (hash = ((PyStringObject *) key)->ob_shash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}
	ep = (mp->ma_lookup)(mp, key, hash);
	return ep == NULL ? -1 : (ep->me_value != NULL);
}

/* Internal version of PyOrderedDict_Contains used when the hash value is already known */
int
_PyOrderedDict_Contains(PyObject *op, PyObject *key, long hash)
{
	PyOrderedDictObject *mp = (PyOrderedDictObject *)op;
	PyOrderedDictEntry *ep;

	ep = (mp->ma_lookup)(mp, key, hash);
	return ep == NULL ? -1 : (ep->me_value != NULL);
}

/* Hack to implement "key in dict" */
static PySequenceMethods dict_as_sequence = {
	0,			/* sq_length */
	0,			/* sq_concat */
	0,			/* sq_repeat */
	0,			/* sq_item */
	0,			/* sq_slice */
	0,			/* sq_ass_item */
	0,			/* sq_ass_slice */
	PyOrderedDict_Contains,	/* sq_contains */
	0,			/* sq_inplace_concat */
	0,			/* sq_inplace_repeat */
};

static PyObject *
dict_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *self;

	assert(type != NULL && type->tp_alloc != NULL);
	assert(dummy != NULL);
	self = type->tp_alloc(type, 0);
	if (self != NULL) {
		PyOrderedDictObject *d = (PyOrderedDictObject *)self;
		/* It's guaranteed that tp->alloc zeroed out the struct. */
		assert(d->ma_table == NULL && d->ma_fill == 0 && d->ma_used == 0);
		INIT_NONZERO_DICT_SLOTS(d);
		d->ma_lookup = lookdict_string;
#ifdef SHOW_CONVERSION_COUNTS
		++created;
#endif
	}
	return self;
}

static int
dict_init(PyObject *self, PyObject *args, PyObject *kwds)
{
	return dict_update_common(self, args, kwds, "dict");
}

static long
dict_nohash(PyObject *self)
{
	PyErr_SetString(PyExc_TypeError, "ordereddict objects are unhashable");
	return -1;
}

static PyObject *
dict_iter(PyOrderedDictObject *dict)
{
	return dictiter_new(dict, &PyOrderedDictIterKey_Type);
}

PyDoc_STRVAR(ordereddict_doc,
"ordereddict() -> new empty dictionary.\n"
//"dict(mapping) -> new dictionary initialized from a mapping object's\n"
//"    (key, value) pairs.\n"
//"dict(seq) -> new dictionary initialized as if via:\n"
//"    d = {}\n"
//"    for k, v in seq:\n"
//"        d[k] = v\n"
//"dict(**kwargs) -> new dictionary initialized with the name=value pairs\n"
//"    in the keyword argument list.  For example:  dict(one=1, two=2)"
);

PyTypeObject PyOrderedDict_Type = {
	PyObject_HEAD_INIT(DEFERRED_ADDRESS(&PyType_Type))
	0,
	"ordereddict",
	sizeof(PyOrderedDictObject),
	0,
	(destructor)dict_dealloc,		/* tp_dealloc */
	(printfunc)dict_print,			/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	(cmpfunc)dict_compare,			/* tp_compare */
	(reprfunc)dict_repr,			/* tp_repr */
	0,					/* tp_as_number */
	&dict_as_sequence,			/* tp_as_sequence */
	&dict_as_mapping,			/* tp_as_mapping */
	dict_nohash,				/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_BASETYPE,		/* tp_flags */
	ordereddict_doc,				/* tp_doc */
	dict_traverse,				/* tp_traverse */
	dict_tp_clear,				/* tp_clear */
	dict_richcompare,			/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	(getiterfunc)dict_iter,			/* tp_iter */
	0,					/* tp_iternext */
	mapp_methods,				/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	DEFERRED_ADDRESS(&PyDict_Type),					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	dict_init,				/* tp_init */
	PyType_GenericAlloc,			/* tp_alloc */
	dict_new,				/* tp_new */
	PyObject_GC_Del,        		/* tp_free */
};

/* Dictionary iterator types */

typedef struct {
	PyObject_HEAD
	PyOrderedDictObject *di_dict; /* Set to NULL when iterator is exhausted */
	Py_ssize_t di_used;
	Py_ssize_t di_pos;
	PyObject* di_result; /* reusable result tuple for iteritems */
	Py_ssize_t len;
} dictiterobject;

static PyObject *
dictiter_new(PyOrderedDictObject *dict, PyTypeObject *itertype)
{
	dictiterobject *di;
	di = PyObject_New(dictiterobject, itertype);
	if (di == NULL)
		return NULL;
	Py_INCREF(dict);
	di->di_dict = dict;
	di->di_used = dict->ma_used;
	di->di_pos = 0;
	di->len = dict->ma_used;
	if (itertype == &PyOrderedDictIterItem_Type) {
		di->di_result = PyTuple_Pack(2, Py_None, Py_None);
		if (di->di_result == NULL) {
			Py_DECREF(di);
			return NULL;
		}
	}
	else
		di->di_result = NULL;
	return (PyObject *)di;
}

static void
dictiter_dealloc(dictiterobject *di)
{
	Py_XDECREF(di->di_dict);
	Py_XDECREF(di->di_result);
	PyObject_Del(di);
}

static PyObject *
dictiter_len(dictiterobject *di)
{
	Py_ssize_t len = 0;
	if (di->di_dict != NULL && di->di_used == di->di_dict->ma_used)
		len = di->len;
	return PyInt_FromSize_t(len);
}

PyDoc_STRVAR(length_hint_doc, "Private method returning an estimate of len(list(it)).");

static PyMethodDef dictiter_methods[] = {
	{"__length_hint__", (PyCFunction)dictiter_len, METH_NOARGS, length_hint_doc},
 	{NULL,		NULL}		/* sentinel */
};

static PyObject *dictiter_iternextkey(dictiterobject *di)
{
	PyObject *key;
	register Py_ssize_t i;
	register PyOrderedDictEntry **epp;
	PyOrderedDictObject *d = di->di_dict;

	if (d == NULL)
		return NULL;
	assert (PyOrderedDict_Check(d));

	if (di->di_used != d->ma_used) {
		PyErr_SetString(PyExc_RuntimeError,
				"dictionary changed size during iteration");
		di->di_used = -1; /* Make this state sticky */
		return NULL;
	}

	i = di->di_pos;
	if (i < 0)
		goto fail;
	if (i >= d->ma_used)
		goto fail;
	epp = d->ma_otablep;
	di->di_pos = i+1;
	di->len--; /* len can be calculated */
	key = epp[i]->me_key;
	Py_INCREF(key);
	return key;

fail:
	Py_DECREF(d);
	di->di_dict = NULL;
	return NULL;
}

PyTypeObject PyOrderedDictIterKey_Type = {
	PyObject_HEAD_INIT(DEFERRED_ADDRESS(&PyType_Type))
	0,					/* ob_size */
	"ordereddict-keyiterator",		/* tp_name */
	sizeof(dictiterobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictiter_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)dictiter_iternextkey,	/* tp_iternext */
	dictiter_methods,			/* tp_methods */
	0,
};

static PyObject *dictiter_iternextvalue(dictiterobject *di)
{
	PyObject *value;
	register Py_ssize_t i;
	register PyOrderedDictEntry **epp;
	PyOrderedDictObject *d = di->di_dict;

	if (d == NULL)
		return NULL;
	assert (PyOrderedDict_Check(d));

	if (di->di_used != d->ma_used) {
		PyErr_SetString(PyExc_RuntimeError,
				"dictionary changed size during iteration");
		di->di_used = -1; /* Make this state sticky */
		return NULL;
	}

	i = di->di_pos;
	if (i < 0 || i >= d->ma_used)
		goto fail;
	epp = d->ma_otablep;
	di->di_pos = i+1;
	di->len--; /* len can be calculated */
	value = epp[i]->me_value;
	Py_INCREF(value);
	return value;

fail:
	Py_DECREF(d);
	di->di_dict = NULL;
	return NULL;
}

PyTypeObject PyOrderedDictIterValue_Type = {
	PyObject_HEAD_INIT(DEFERRED_ADDRESS(&PyType_Type))
	0,					/* ob_size */
	"ordereddict-valueiterator",		/* tp_name */
	sizeof(dictiterobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictiter_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)dictiter_iternextvalue,	/* tp_iternext */
	dictiter_methods,			/* tp_methods */
	0,
};

static PyObject *dictiter_iternextitem(dictiterobject *di)
{
	PyObject *key, *value, *result = di->di_result;
	register Py_ssize_t i;
	register PyOrderedDictEntry **epp;
	PyOrderedDictObject *d = di->di_dict;

	if (d == NULL)
		return NULL;
	assert (PyOrderedDict_Check(d));

	if (di->di_used != d->ma_used) {
		PyErr_SetString(PyExc_RuntimeError,
				"dictionary changed size during iteration");
		di->di_used = -1; /* Make this state sticky */
		return NULL;
	}

	i = di->di_pos;
	if (i < 0)
		goto fail;

	if (i >= d->ma_used)
		goto fail;
	epp = d->ma_otablep;
	di->di_pos = i+1;
	if (result->ob_refcnt == 1) {
		Py_INCREF(result);
		Py_DECREF(PyTuple_GET_ITEM(result, 0));
		Py_DECREF(PyTuple_GET_ITEM(result, 1));
	} else {
		result = PyTuple_New(2);
		if (result == NULL)
			return NULL;
	}
	di->len--; /* len can be calculated */
	key = epp[i]->me_key;
	value = epp[i]->me_value;
	Py_INCREF(key);
	Py_INCREF(value);
	PyTuple_SET_ITEM(result, 0, key);
	PyTuple_SET_ITEM(result, 1, value);
	return result;

fail:
	Py_DECREF(d);
	di->di_dict = NULL;
	return NULL;
}

PyTypeObject PyOrderedDictIterItem_Type = {
	PyObject_HEAD_INIT(DEFERRED_ADDRESS(&PyType_Type))
	0,					/* ob_size */
	"ordereddict-itemiterator",		/* tp_name */
	sizeof(dictiterobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictiter_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)dictiter_iternextitem,	/* tp_iternext */
	dictiter_methods,			/* tp_methods */
	0,
};

/*******************************************************************/

static PyMethodDef ordereddict_functions[] = {
/*	{"bench",	ordered_bench, 	METH_VARARGS,	"benchmarking func"}, */
	{NULL,		NULL}		/* sentinel */
};


PyMODINIT_FUNC
initordereddict(void)
{
	PyObject *m;
	
	if (dummy == NULL) { /* Auto-initialize dummy */
		dummy = PyString_FromString("<dummy key>");
		if (dummy == NULL)
			return;
#ifdef SHOW_CONVERSION_COUNTS
		Py_AtExit(show_counts);
#endif
	}

	/* Fill in deferred data addresses.  This must be done before
	   PyType_Ready() is called.  Note that PyType_Ready() automatically
	   initializes the ob.ob_type field to &PyType_Type if it's NULL,
	   so it's not necessary to fill in ob_type first. */
	PyOrderedDict_Type.tp_base = &PyDict_Type;

	if (PyType_Ready(&PyOrderedDict_Type) < 0)
		return;
	
	/* AvdN: TODO understand why it is necessary or not (as it seems)
	to PyTypeReady the iterator types
	*/

	m = Py_InitModule3("ordereddict",
			   ordereddict_functions,
			   ordereddict_doc
                          // , NULL, PYTHON_API_VERSION
                           );
	if (m == NULL)
		return;

	if (PyType_Ready(&PyOrderedDict_Type) < 0)
		return;

	Py_INCREF(&PyOrderedDict_Type);
	if (PyModule_AddObject(m, "ordereddict",
			       (PyObject *) &PyOrderedDict_Type) < 0)
		return;
}