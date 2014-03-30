/*
 * Copyright (c) 2014, Vsevolod Stakhov
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ucl.h"
#include "ucl_internal.h"
#include "tree.h"
#include "utlist.h"
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif
#ifdef HAVE_REGEX_H
#include <regex.h>
#endif
#ifdef HAVE_MATH_H
#include <math.h>
#endif

static bool ucl_schema_validate (ucl_object_t *schema,
		ucl_object_t *obj, bool try_array,
		struct ucl_schema_error *err,
		ucl_object_t *root);

static bool
ucl_string_to_type (const char *input, ucl_type_t *res)
{
	if (strcasecmp (input, "object") == 0) {
		*res = UCL_OBJECT;
	}
	else if (strcasecmp (input, "array") == 0) {
		*res = UCL_ARRAY;
	}
	else if (strcasecmp (input, "integer") == 0) {
		*res = UCL_INT;
	}
	else if (strcasecmp (input, "number") == 0) {
		*res = UCL_FLOAT;
	}
	else if (strcasecmp (input, "string") == 0) {
		*res = UCL_STRING;
	}
	else if (strcasecmp (input, "boolean") == 0) {
		*res = UCL_BOOLEAN;
	}
	else if (strcasecmp (input, "null") == 0) {
		*res = UCL_NULL;
	}
	else {
		return false;
	}

	return true;
}

static const char *
ucl_object_type_to_string (ucl_type_t type)
{
	const char *res = "unknown";

	switch (type) {
	case UCL_OBJECT:
		res = "object";
		break;
	case UCL_ARRAY:
		res = "array";
		break;
	case UCL_INT:
		res = "integer";
		break;
	case UCL_FLOAT:
	case UCL_TIME:
		res = "number";
		break;
	case UCL_STRING:
		res = "string";
		break;
	case UCL_BOOLEAN:
		res = "boolean";
		break;
	case UCL_NULL:
	case UCL_USERDATA:
		res = "null";
		break;
	}

	return res;
}

/*
 * Create validation error
 */
static void
ucl_schema_create_error (struct ucl_schema_error *err,
		enum ucl_schema_error_code code, ucl_object_t *obj,
		const char *fmt, ...)
{
	va_list va;

	if (err != NULL) {
		err->code = code;
		err->obj = obj;
		va_start (va, fmt);
		vsnprintf (err->msg, sizeof (err->msg), fmt, va);
		va_end (va);
	}
}

/*
 * Check whether we have a pattern specified
 */
static ucl_object_t *
ucl_schema_test_pattern (ucl_object_t *obj, const char *pattern)
{
	regex_t reg;
	ucl_object_t *res = NULL, *elt;
	ucl_object_iter_t iter = NULL;

	if (regcomp (&reg, pattern, REG_EXTENDED | REG_NOSUB) == 0) {
		while ((elt = ucl_iterate_object (obj, &iter, true)) != NULL) {
			if (regexec (&reg, ucl_object_key (elt), 0, NULL, 0) == 0) {
				res = elt;
				break;
			}
		}
		regfree (&reg);
	}

	return res;
}

/*
 * Check dependencies for an object
 */
static bool
ucl_schema_validate_dependencies (ucl_object_t *deps,
		ucl_object_t *obj, struct ucl_schema_error *err,
		ucl_object_t *root)
{
	ucl_object_t *elt, *cur, *cur_dep;
	ucl_object_iter_t iter = NULL, piter;
	bool ret = true;

	while (ret && (cur = ucl_iterate_object (deps, &iter, true)) != NULL) {
		elt = ucl_object_find_key (obj, ucl_object_key (cur));
		if (elt != NULL) {
			/* Need to check dependencies */
			if (cur->type == UCL_ARRAY) {
				piter = NULL;
				while (ret && (cur_dep = ucl_iterate_object (cur, &piter, true)) != NULL) {
					if (ucl_object_find_key (obj, ucl_object_tostring (cur_dep)) == NULL) {
						ucl_schema_create_error (err, UCL_SCHEMA_MISSING_DEPENDENCY, elt,
								"dependency %s is missing for key %s",
								ucl_object_tostring (cur_dep), ucl_object_key (cur));
						ret = false;
						break;
					}
				}
			}
			else if (cur->type == UCL_OBJECT) {
				ret = ucl_schema_validate (cur, obj, true, err, root);
			}
		}
	}

	return ret;
}

/*
 * Validate object
 */
static bool
ucl_schema_validate_object (ucl_object_t *schema,
		ucl_object_t *obj, struct ucl_schema_error *err,
		ucl_object_t *root)
{
	ucl_object_t *elt, *prop, *found, *additional_schema = NULL,
			*required = NULL, *pat, *pelt;
	ucl_object_iter_t iter = NULL, piter = NULL;
	bool ret = true, allow_additional = true;
	int64_t minmax;

	while (ret && (elt = ucl_iterate_object (schema, &iter, true)) != NULL) {
		if (elt->type == UCL_OBJECT &&
				strcmp (ucl_object_key (elt), "properties") == 0) {
			piter = NULL;
			while (ret && (prop = ucl_iterate_object (elt, &piter, true)) != NULL) {
				found = ucl_object_find_key (obj, ucl_object_key (prop));
				if (found) {
					ret = ucl_schema_validate (prop, found, true, err, root);
				}
			}
		}
		else if (strcmp (ucl_object_key (elt), "additionalProperties") == 0) {
			if (elt->type == UCL_BOOLEAN) {
				if (!ucl_object_toboolean (elt)) {
					/* Deny additional fields completely */
					allow_additional = false;
				}
			}
			else if (elt->type == UCL_OBJECT) {
				/* Define validator for additional fields */
				additional_schema = elt;
			}
			else {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, elt,
						"additionalProperties attribute is invalid in schema");
				ret = false;
				break;
			}
		}
		else if (strcmp (ucl_object_key (elt), "required") == 0) {
			if (elt->type == UCL_ARRAY) {
				required = elt;
			}
			else {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, elt,
						"required attribute is invalid in schema");
				ret = false;
				break;
			}
		}
		else if (strcmp (ucl_object_key (elt), "minProperties") == 0
				&& ucl_object_toint_safe (elt, &minmax)) {
			if (obj->len < minmax) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"object has not enough properties: %u, minimum is: %u",
						obj->len, (unsigned)minmax);
				ret = false;
				break;
			}
		}
		else if (strcmp (ucl_object_key (elt), "maxProperties") == 0
				&& ucl_object_toint_safe (elt, &minmax)) {
			if (obj->len > minmax) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"object has too many properties: %u, maximum is: %u",
						obj->len, (unsigned)minmax);
				ret = false;
				break;
			}
		}
		else if (strcmp (ucl_object_key (elt), "patternProperties") == 0) {
			piter = NULL;
			while (ret && (prop = ucl_iterate_object (elt, &piter, true)) != NULL) {
				found = ucl_schema_test_pattern (obj, ucl_object_key (prop));
				if (found) {
					ret = ucl_schema_validate (prop, found, true, err, root);
				}
			}
		}
		else if (elt->type == UCL_OBJECT &&
				strcmp (ucl_object_key (elt), "dependencies") == 0) {
			ret = ucl_schema_validate_dependencies (elt, obj, err, root);
		}
	}

	if (ret) {
		/* Additional properties */
		if (!allow_additional || additional_schema != NULL) {
			/* Check if we have exactly the same properties in schema and object */
			iter = NULL;
			prop = ucl_object_find_key (schema, "properties");
			while ((elt = ucl_iterate_object (obj, &iter, true)) != NULL) {
				found = ucl_object_find_key (prop, ucl_object_key (elt));
				if (found == NULL) {
					/* Try patternProperties */
					piter = NULL;
					pat = ucl_object_find_key (schema, "patternProperties");
					while ((pelt = ucl_iterate_object (pat, &piter, true)) != NULL) {
						found = ucl_schema_test_pattern (obj, ucl_object_key (pelt));
						if (found != NULL) {
							break;
						}
					}
				}
				if (found == NULL) {
					if (!allow_additional) {
						ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
								"object has non-allowed property %s",
								ucl_object_key (elt));
						ret = false;
						break;
					}
					else if (additional_schema != NULL) {
						if (!ucl_schema_validate (additional_schema, elt, true, err, root)) {
							ret = false;
							break;
						}
					}
				}
			}
		}
		/* Required properties */
		if (required != NULL) {
			iter = NULL;
			while ((elt = ucl_iterate_object (required, &iter, true)) != NULL) {
				if (ucl_object_find_key (obj, ucl_object_tostring (elt)) == NULL) {
					ucl_schema_create_error (err, UCL_SCHEMA_MISSING_PROPERTY, obj,
							"object has missing property %s",
							ucl_object_tostring (elt));
					ret = false;
					break;
				}
			}
		}
	}


	return ret;
}

static bool
ucl_schema_validate_number (ucl_object_t *schema,
		ucl_object_t *obj, struct ucl_schema_error *err)
{
	ucl_object_t *elt, *test;
	ucl_object_iter_t iter = NULL;
	bool ret = true, exclusive = false;
	double constraint, val;
	const double alpha = 1e-16;

	while (ret && (elt = ucl_iterate_object (schema, &iter, true)) != NULL) {
		if ((elt->type == UCL_FLOAT || elt->type == UCL_INT) &&
				strcmp (ucl_object_key (elt), "multipleOf") == 0) {
			constraint = ucl_object_todouble (elt);
			if (constraint <= 0) {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, elt,
						"multipleOf must be greater than zero");
				ret = false;
				break;
			}
			val = ucl_object_todouble (obj);
			if (fabs (remainder (val, constraint)) > alpha) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"number %.4f is not multiple of %.4f, remainder is %.7f",
						val, constraint);
				ret = false;
				break;
			}
		}
		else if ((elt->type == UCL_FLOAT || elt->type == UCL_INT) &&
			strcmp (ucl_object_key (elt), "maximum") == 0) {
			constraint = ucl_object_todouble (elt);
			test = ucl_object_find_key (schema, "exclusiveMaximum");
			if (test && test->type == UCL_BOOLEAN) {
				exclusive = ucl_object_toboolean (test);
			}
			val = ucl_object_todouble (obj);
			if (val > constraint || (exclusive && val >= constraint)) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"number is too big: %.3f, maximum is: %.3f",
						val, constraint);
				ret = false;
				break;
			}
		}
		else if ((elt->type == UCL_FLOAT || elt->type == UCL_INT) &&
				strcmp (ucl_object_key (elt), "minimum") == 0) {
			constraint = ucl_object_todouble (elt);
			test = ucl_object_find_key (schema, "exclusiveMinimum");
			if (test && test->type == UCL_BOOLEAN) {
				exclusive = ucl_object_toboolean (test);
			}
			val = ucl_object_todouble (obj);
			if (val < constraint || (exclusive && val <= constraint)) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"number is too small: %.3f, minimum is: %.3f",
						val, constraint);
				ret = false;
				break;
			}
		}
	}

	return ret;
}

static bool
ucl_schema_validate_string (ucl_object_t *schema,
		ucl_object_t *obj, struct ucl_schema_error *err)
{
	ucl_object_t *elt;
	ucl_object_iter_t iter = NULL;
	bool ret = true;
	int64_t constraint;
	regex_t re;

	while (ret && (elt = ucl_iterate_object (schema, &iter, true)) != NULL) {
		if (elt->type == UCL_INT &&
			strcmp (ucl_object_key (elt), "maxLength") == 0) {
			constraint = ucl_object_toint (elt);
			if (obj->len > constraint) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"string is too big: %.3f, maximum is: %.3f",
						obj->len, constraint);
				ret = false;
				break;
			}
		}
		else if (elt->type == UCL_INT &&
				strcmp (ucl_object_key (elt), "minLength") == 0) {
			constraint = ucl_object_toint (elt);
			if (obj->len < constraint) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"string is too short: %.3f, minimum is: %.3f",
						obj->len, constraint);
				ret = false;
				break;
			}
		}
		else if (elt->type == UCL_STRING &&
				strcmp (ucl_object_key (elt), "pattern") == 0) {
			if (regcomp (&re, ucl_object_tostring (elt),
					REG_EXTENDED | REG_NOSUB) != 0) {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, elt,
						"cannot compile pattern %s", ucl_object_tostring (elt));
				ret = false;
				break;
			}
			if (regexec (&re, ucl_object_tostring (obj), 0, NULL, 0) != 0) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"string doesn't match regexp %s",
						ucl_object_tostring (elt));
				ret = false;
			}
			regfree (&re);
		}
	}

	return ret;
}

struct ucl_compare_node {
	ucl_object_t *obj;
	TREE_ENTRY(ucl_compare_node) link;
	struct ucl_compare_node *next;
};

typedef TREE_HEAD(_tree, ucl_compare_node) ucl_compare_tree_t;

TREE_DEFINE(ucl_compare_node, link)

static int
ucl_schema_elt_compare (struct ucl_compare_node *n1, struct ucl_compare_node *n2)
{
	ucl_object_t *o1 = n1->obj, *o2 = n2->obj;

	return ucl_object_compare (o1, o2);
}

static bool
ucl_schema_array_is_unique (ucl_object_t *obj, struct ucl_schema_error *err)
{
	ucl_compare_tree_t tree = TREE_INITIALIZER (ucl_schema_elt_compare);
	ucl_object_iter_t iter = NULL;
	ucl_object_t *elt;
	struct ucl_compare_node *node, test, *nodes = NULL, *tmp;
	bool ret = true;

	while ((elt = ucl_iterate_object (obj, &iter, true)) != NULL) {
		test.obj = elt;
		node = TREE_FIND (&tree, ucl_compare_node, link, &test);
		if (node != NULL) {
			ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, elt,
					"duplicate values detected while uniqueItems is true");
			ret = false;
			break;
		}
		node = calloc (1, sizeof (*node));
		if (node == NULL) {
			ucl_schema_create_error (err, UCL_SCHEMA_UNKNOWN, elt,
					"cannot allocate tree node");
			ret = false;
			break;
		}
		node->obj = elt;
		TREE_INSERT (&tree, ucl_compare_node, link, node);
		LL_PREPEND (nodes, node);
	}

	LL_FOREACH_SAFE (nodes, node, tmp) {
		free (node);
	}

	return ret;
}

static bool
ucl_schema_validate_array (ucl_object_t *schema,
		ucl_object_t *obj, struct ucl_schema_error *err,
		ucl_object_t *root)
{
	ucl_object_t *elt, *it, *found, *additional_schema = NULL,
			*first_unvalidated = NULL;
	ucl_object_iter_t iter = NULL, piter = NULL;
	bool ret = true, allow_additional = true, need_unique = false;
	int64_t minmax;

	while (ret && (elt = ucl_iterate_object (schema, &iter, true)) != NULL) {
		if (strcmp (ucl_object_key (elt), "items") == 0) {
			if (elt->type == UCL_ARRAY) {
				found = obj->value.av;
				while (ret && (it = ucl_iterate_object (elt, &piter, true)) != NULL) {
					if (found) {
						ret = ucl_schema_validate (it, found, false, err, root);
						found = found->next;
					}
				}
				if (found != NULL) {
					/* The first element that is not validated */
					first_unvalidated = found;
				}
			}
			else if (elt->type == UCL_OBJECT) {
				/* Validate all items using the specified schema */
				while (ret && (it = ucl_iterate_object (obj, &piter, true)) != NULL) {
					ret = ucl_schema_validate (elt, it, false, err, root);
				}
			}
			else {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, elt,
						"items attribute is invalid in schema");
				ret = false;
				break;
			}
		}
		else if (strcmp (ucl_object_key (elt), "additionalItems") == 0) {
			if (elt->type == UCL_BOOLEAN) {
				if (!ucl_object_toboolean (elt)) {
					/* Deny additional fields completely */
					allow_additional = false;
				}
			}
			else if (elt->type == UCL_OBJECT) {
				/* Define validator for additional fields */
				additional_schema = elt;
			}
			else {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, elt,
						"additionalItems attribute is invalid in schema");
				ret = false;
				break;
			}
		}
		else if (elt->type == UCL_BOOLEAN &&
				strcmp (ucl_object_key (elt), "uniqueItems") == 0) {
			need_unique = ucl_object_toboolean (elt);
		}
		else if (strcmp (ucl_object_key (elt), "minItems") == 0
				&& ucl_object_toint_safe (elt, &minmax)) {
			if (obj->len < minmax) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"array has not enough items: %u, minimum is: %u",
						obj->len, (unsigned)minmax);
				ret = false;
				break;
			}
		}
		else if (strcmp (ucl_object_key (elt), "maxItems") == 0
				&& ucl_object_toint_safe (elt, &minmax)) {
			if (obj->len > minmax) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
						"array has too many items: %u, maximum is: %u",
						obj->len, (unsigned)minmax);
				ret = false;
				break;
			}
		}
	}

	if (ret) {
		/* Additional properties */
		if (!allow_additional || additional_schema != NULL) {
			if (first_unvalidated != NULL) {
				if (!allow_additional) {
					ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
							"array has undefined item");
					ret = false;
				}
				else if (additional_schema != NULL) {
					elt = first_unvalidated;
					while (elt) {
						if (!ucl_schema_validate (additional_schema, elt, false,
								err, root)) {
							ret = false;
							break;
						}
						elt = elt->next;
					}
				}
			}
		}
		/* Required properties */
		if (ret && need_unique) {
			ret = ucl_schema_array_is_unique (obj, err);
		}
	}

	return ret;
}

/*
 * Returns whether this object is allowed for this type
 */
static bool
ucl_schema_type_is_allowed (ucl_object_t *type, ucl_object_t *obj,
		struct ucl_schema_error *err)
{
	ucl_object_iter_t iter = NULL;
	ucl_object_t *elt;
	const char *type_str;
	ucl_type_t t;

	if (type == NULL) {
		/* Any type is allowed */
		return true;
	}

	if (type->type == UCL_ARRAY) {
		/* One of allowed types */
		while ((elt = ucl_iterate_object (type, &iter, true)) != NULL) {
			if (ucl_schema_type_is_allowed (elt, obj, err)) {
				return true;
			}
		}
	}
	else if (type->type == UCL_STRING) {
		type_str = ucl_object_tostring (type);
		if (!ucl_string_to_type (type_str, &t)) {
			ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, type,
					"Type attribute is invalid in schema");
			return false;
		}
		if (obj->type != t) {
			/* Some types are actually compatible */
			if (obj->type == UCL_TIME && t == UCL_FLOAT) {
				return true;
			}
			else if (obj->type == UCL_INT && t == UCL_FLOAT) {
				return true;
			}
			else {
				ucl_schema_create_error (err, UCL_SCHEMA_TYPE_MISMATCH, obj,
						"Invalid type of %s, expected %s",
						ucl_object_type_to_string (obj->type),
						ucl_object_type_to_string (t));
			}
		}
		else {
			/* Types are equal */
			return true;
		}
	}

	return false;
}

/*
 * Check if object is equal to one of elements of enum
 */
static bool
ucl_schema_validate_enum (ucl_object_t *en, ucl_object_t *obj,
		struct ucl_schema_error *err)
{
	ucl_object_iter_t iter = NULL;
	ucl_object_t *elt;
	bool ret = false;

	while ((elt = ucl_iterate_object (en, &iter, true)) != NULL) {
		if (ucl_object_compare (elt, obj) == 0) {
			ret = true;
			break;
		}
	}

	if (!ret) {
		ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
				"object is not one of enumerated patterns");
	}

	return ret;
}


/*
 * Check a single ref component
 */
static ucl_object_t *
ucl_schema_resolve_ref_component (ucl_object_t *cur,
		const char *refc, int len,
		struct ucl_schema_error *err)
{
	ucl_object_t *res = NULL;
	char *err_str;
	int num, i;

	if (cur->type == UCL_OBJECT) {
		/* Find a key inside an object */
		res = ucl_object_find_keyl (cur, refc, len);
		if (res == NULL) {
			ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, cur,
					"reference %s is invalid, missing path component", refc);
			return NULL;
		}
	}
	else if (cur->type == UCL_ARRAY) {
		/* We must figure out a number inside array */
		num = strtoul (refc, &err_str, 10);
		if (err_str != NULL && *err_str != '/' && *err_str != '\0') {
			ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, cur,
					"reference %s is invalid, invalid item number", refc);
			return NULL;
		}
		res = cur->value.av;
		i = 0;
		while (res != NULL) {
			if (i == num) {
				break;
			}
			res = res->next;
		}
		if (res == NULL) {
			ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, cur,
					"reference %s is invalid, item number %d does not exist",
					refc, num);
			return NULL;
		}
	}
	else {
		ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, res,
				"reference %s is invalid, contains primitive object in the path",
				refc);
		return NULL;
	}

	return res;
}
/*
 * Find reference schema
 */
static ucl_object_t *
ucl_schema_resolve_ref (ucl_object_t *root, const char *ref,
		struct ucl_schema_error *err)
{
	const char *p, *c;
	ucl_object_t *res = NULL;


	if (ref[0] != '#') {
		ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, root,
				"reference %s is invalid, not started with #", ref);
		return NULL;
	}
	if (ref[1] == '/') {
		p = &ref[2];
	}
	else if (ref[1] == '\0') {
		return root;
	}
	else {
		ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, root,
				"reference %s is invalid, not started with #/", ref);
		return NULL;
	}

	c = p;
	res = root;

	while (*p != '\0') {
		if (*p == '/') {
			if (p - c == 0) {
				ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, res,
						"reference %s is invalid, empty path component", ref);
				return NULL;
			}
			/* Now we have some url part, so we need to figure out where we are */
			res = ucl_schema_resolve_ref_component (res, c, p - c, err);
			if (res == NULL) {
				return NULL;
			}
			c = p + 1;
		}
		p ++;
	}

	if (p - c != 0) {
		res = ucl_schema_resolve_ref_component (res, c, p - c, err);
	}

	if (res == NULL || res->type != UCL_OBJECT) {
		ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, res,
				"reference %s is invalid, cannot find specified object",
				ref);
		return NULL;
	}

	return res;
}

static bool
ucl_schema_validate_values (ucl_object_t *schema, ucl_object_t *obj,
		struct ucl_schema_error *err)
{
	ucl_object_t *elt, *cur;
	int64_t constraint, i;

	elt = ucl_object_find_key (schema, "maxValues");
	if (elt != NULL && elt->type == UCL_INT) {
		constraint = ucl_object_toint (elt);
		cur = obj;
		i = 0;
		while (cur) {
			if (i > constraint) {
				ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
					"object has more values than defined: %ld",
					(long int)constraint);
				return false;
			}
			i ++;
			cur = cur->next;
		}
	}
	elt = ucl_object_find_key (schema, "minValues");
	if (elt != NULL && elt->type == UCL_INT) {
		constraint = ucl_object_toint (elt);
		cur = obj;
		i = 0;
		while (cur) {
			if (i >= constraint) {
				break;
			}
			i ++;
			cur = cur->next;
		}
		if (i < constraint) {
			ucl_schema_create_error (err, UCL_SCHEMA_CONSTRAINT, obj,
					"object has less values than defined: %ld",
					(long int)constraint);
			return false;
		}
	}

	return true;
}

static bool
ucl_schema_validate (ucl_object_t *schema,
		ucl_object_t *obj, bool try_array,
		struct ucl_schema_error *err,
		ucl_object_t *root)
{
	ucl_object_t *elt, *cur;
	ucl_object_iter_t iter = NULL;
	bool ret;

	if (schema->type != UCL_OBJECT) {
		ucl_schema_create_error (err, UCL_SCHEMA_INVALID_SCHEMA, schema,
				"schema is %s instead of object", ucl_object_type_to_string (schema->type));
		return false;
	}

	if (try_array) {
		/*
		 * Special case for multiple values
		 */
		if (!ucl_schema_validate_values (schema, obj, err)) {
			return false;
		}
		LL_FOREACH (obj, cur) {
			if (!ucl_schema_validate (schema, cur, false, err, root)) {
				return false;
			}
		}
		return true;
	}

	elt = ucl_object_find_key (schema, "enum");
	if (elt != NULL && elt->type == UCL_ARRAY) {
		if (!ucl_schema_validate_enum (elt, obj, err)) {
			return false;
		}
	}

	elt = ucl_object_find_key (schema, "allOf");
	if (elt != NULL && elt->type == UCL_ARRAY) {
		iter = NULL;
		while ((cur = ucl_iterate_object (elt, &iter, true)) != NULL) {
			ret = ucl_schema_validate (cur, obj, true, err, root);
			if (!ret) {
				return false;
			}
		}
	}

	elt = ucl_object_find_key (schema, "anyOf");
	if (elt != NULL && elt->type == UCL_ARRAY) {
		iter = NULL;
		while ((cur = ucl_iterate_object (elt, &iter, true)) != NULL) {
			ret = ucl_schema_validate (cur, obj, true, err, root);
			if (ret) {
				break;
			}
		}
		if (!ret) {
			return false;
		}
		else {
			/* Reset error */
			err->code = UCL_SCHEMA_OK;
		}
	}

	elt = ucl_object_find_key (schema, "oneOf");
	if (elt != NULL && elt->type == UCL_ARRAY) {
		iter = NULL;
		ret = false;
		while ((cur = ucl_iterate_object (elt, &iter, true)) != NULL) {
			if (!ret) {
				ret = ucl_schema_validate (cur, obj, true, err, root);
			}
			else if (ucl_schema_validate (cur, obj, true, err, root)) {
				ret = false;
				break;
			}
		}
		if (!ret) {
			return false;
		}
	}

	elt = ucl_object_find_key (schema, "not");
	if (elt != NULL && elt->type == UCL_OBJECT) {
		if (ucl_schema_validate (elt, obj, true, err, root)) {
			return false;
		}
		else {
			/* Reset error */
			err->code = UCL_SCHEMA_OK;
		}
	}

	elt = ucl_object_find_key (schema, "$ref");
	if (elt != NULL) {
		cur = ucl_schema_resolve_ref (root, ucl_object_tostring (elt), err);
		if (cur == NULL) {
			return false;
		}
		if (!ucl_schema_validate (cur, obj, try_array, err, root)) {
			return false;
		}
	}

	elt = ucl_object_find_key (schema, "type");
	if (!ucl_schema_type_is_allowed (elt, obj, err)) {
		return false;
	}

	switch (obj->type) {
	case UCL_OBJECT:
		return ucl_schema_validate_object (schema, obj, err, root);
		break;
	case UCL_ARRAY:
		return ucl_schema_validate_array (schema, obj, err, root);
		break;
	case UCL_INT:
	case UCL_FLOAT:
		return ucl_schema_validate_number (schema, obj, err);
		break;
	case UCL_STRING:
		return ucl_schema_validate_string (schema, obj, err);
		break;
	default:
		break;
	}

	return true;
}

bool
ucl_object_validate (ucl_object_t *schema,
		ucl_object_t *obj, struct ucl_schema_error *err)
{
	return ucl_schema_validate (schema, obj, true, err, schema);
}
