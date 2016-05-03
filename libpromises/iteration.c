/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <iteration.h>

#include <scope.h>
#include <vars.h>
#include <fncall.h>
#include <eval_context.h>
#include <misc_lib.h>
#include <string_lib.h>
#include <assoc.h>
#include <expand.h>


typedef struct {
    /* The unexpanded variable name, dependent on inner expansions. This
     * field never changes after Wheel initialisation. */
    char *varname_unexp;
    /* Number of dependencies of varname_unexp */
//    const size_t dependencies;
    /* On each iteration of the wheels, the unexpanded string is
     * re-expanded, so the following is refilled, again and again. */
    char *varname_exp;
    /* Values of varname_exp, to iterate on. WE DO NOT OWN THE RVALS, they
     * belong to EvalContext, so don't free(). Only if type is
     * container-converted-to-list we own the strings. For more type info see
     * vartype. */
    /* TODO MAYBE WE SHOULD DUPLICATE AND OWN ALL VALUES in order to avoid
       the nasty case of variable changing while iterating? */
    Seq *values;
    /* This is the list-type of the iterable variable, and this sets the type
     * of the elements stored in Seq values. Only possibilities are INTLIST,
     * REALLIST, SLIST (containers get converted to slists). */
    DataType vartype;
    /* Wheel size: the value of the variable varname_exp can be a scalar
     * (wheel size 1) or an slist/container (wheel size possibly greater). So
     * this is basically the length of the "values" field. */
//    size_t size;                          SeqLength
    size_t iter_index;
} Wheel;


typedef struct PromiseIterator_ {
    Seq *wheels;
    const Promise *pp;                                   /* not owned by us */
    size_t count;                                       /* iterations count */
} PromiseIterator;


/**
 * @NOTE #varname doesn't need to be '\0'-terminated, since the length is
 *                provided.
 */
Wheel *WheelNew(const char *varname, size_t varname_len)
{
    Wheel new_wheel = {
        .varname_unexp = xstrndup(varname, varname_len),
//        .varname_exp   = BufferNewWithCapacity(strlen(varname) * 2),
        .varname_exp   = NULL,
        .values        = NULL,
        .vartype       = -1,
        .iter_index    = 0
    };

    return xmemdup(&new_wheel, sizeof(new_wheel));
}

static void WheelValuesSeqDestroy(Wheel *w)
{
    if (w->values != NULL)
    {
        /* Only if it is a container we need to free the values, since it was
         * trasformed to a Seq of strings. */
        if (w->vartype == CF_DATA_TYPE_CONTAINER)
        {
            size_t values_len = SeqLength(w->values);
            for (size_t i = 0; i < values_len; i++)
            {
                char *value = SeqAt(w->values, i);
                free(value);
            }
        }
        SeqDestroy(w->values);
    }
}

void WheelDestroy(void *wheel)
{
    Wheel *w = wheel;
    free(w->varname_unexp);
//    BufferDestroy(w->varname_exp);
    free(w->varname_exp);
    WheelValuesSeqDestroy(w);
    free(w);
}

int WheelCompareUnexpanded(const void *wheel1, const void *wheel2,
                           void *user_data ARG_UNUSED)
{
    const Wheel *w1 = wheel1;
    const Wheel *w2 = wheel2;
    return strcmp(w1->varname_unexp, w2->varname_unexp);
}

PromiseIterator *PromiseIteratorNew(const Promise *pp)
{
    Log(LOG_LEVEL_DEBUG, "PromiseIteratorNew()");
    PromiseIterator iterctx = {
        .wheels = SeqNew(4, WheelDestroy),
        .pp     = pp,
        .count  = 0
    };
    return xmemdup(&iterctx, sizeof(iterctx));
}

void PromiseIteratorDestroy(PromiseIterator *iterctx)
{
    SeqDestroy(iterctx->wheels);
    free(iterctx);
}

size_t PromiseIteratorIndex(const PromiseIterator *iter_ctx)
{
    return iter_ctx->count;
}


/**
 * Returns offset to "$(" or "${" in the string. If not found, then the offset
 * points to the terminating '\0' of the string.
 */
static size_t FindDollarParen(const char *s)
{
    size_t i = 0;

    while (s[i] != '\0' &&
           ! (s[i] == '$' && (s[i+1] == '(' || s[i+1] == '{')))
    {
        i++;
    }
    return i;
}

static char opposite(char c)
{
    switch (c)
    {
    case '(':  return ')';
    case '{':  return '}';
    default :  ProgrammingError("Was expecting '(' or '{' but got: '%c'", c);
    }
    return 0;
}

static bool IsMangled(const char *s)
{
    size_t dollar_paren = FindDollarParen(s);
    size_t bracket      = strchrnul(s, '[') - s;
    size_t upto = MIN(dollar_paren, bracket);
    size_t mangled_ns     = strchrnul(s, CF_MANGLED_NS)    - s;
    size_t mangled_scope  = strchrnul(s, CF_MANGLED_SCOPE) - s;

    if (mangled_ns    < upto ||
        mangled_scope < upto)
    {
        return true;
    }
    else
    {
        return false;
    }
}

static void MangleVarRefString(char *ref_str, size_t len)
{
//    printf("MangleVarRefString: %.*s\n", (int) len, ref_str);

    /* Mangle up to '$(', '${', '[', '\0', whichever comes first. */
    size_t dollar_paren = FindDollarParen(ref_str);
    size_t upto         = MIN(len, dollar_paren);
    char *bracket       = memchr(ref_str, '[', upto);
    if (bracket != NULL)
    {
        upto = bracket - ref_str;
    }

    char *ns = memchr(ref_str, ':', upto);
    char *ref_str2 = ref_str;
    if (ns != NULL)
    {
        *ns      = CF_MANGLED_NS;
        ref_str2 =  ns + 1;
        upto    -= (ns + 1 - ref_str);
        assert(upto >= 0);
    }

    bool mangled_scope = false;
    char *scope = memchr(ref_str2, '.', upto);
    if (scope != NULL         &&
        scope - ref_str2 != 4 &&
        strncmp(ref_str2, "this", 4) != 0)
    {
        *scope = CF_MANGLED_SCOPE;
        mangled_scope = true;
    }

    if (mangled_scope || ns != NULL)
    {
        Log(LOG_LEVEL_DEBUG,
            "Mangled namespaced/scoped variable for iterating over it: %.*s",
            (int) len, ref_str);
    }
}

#if 0
static void DeMangleVarRefString(char *ref_str, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (ref_str[i] == CF_MANGLED_NS)
        {
            ref_str[i] = ':';
        }
        else if (ref_str[i] == CF_MANGLED_SCOPE)
        {
            ref_str[i] = '.';
        }
        else if (ref_str[i] == '[')
        {
            return;
        }
    }
}
#endif

bool IsIterable(DataType t)
{
    if (t == CF_DATA_TYPE_STRING_LIST ||
        t == CF_DATA_TYPE_INT_LIST    ||
        t == CF_DATA_TYPE_REAL_LIST   ||
        t == CF_DATA_TYPE_CONTAINER)
    {
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * Recursive function that adds wheels to the iteration engine, according to
 * the variable (and possibly its inner variables) in #s.
 *
 * Another important thing it does, is *modify* the string #s, mangling all
 * scoped or namespaces variable names.
 *
 * @TODO mangle only the iterables.
 *
 * @param s is the start of a variable, right after "$(" or "${".
 * @param c is the character after '$', i.e. must be either '(' or '{'.
 * @return pointer to the closing parenthesis or brace of the variable, or
 *         if not found, returns a pointer to terminating '\0' of #s.
 */
static char *ProcessVar(PromiseIterator *iterctx, char *s, char c)
{
    char closing_paren   = opposite(c);
    char *s_end    = strchrnul(s, closing_paren);
    char *next_var = s + FindDollarParen(s);
    size_t dependencies  = 0;

    while (next_var < s_end)             /* does it have nested variables?  */
    {
        /* It's a dependent variable, the wheels of the dependencies must be
         * added first. Example: "$(blah_$(dependency))" */

        assert(next_var[0] != '\0');
        assert(next_var[1] != '\0');

        char *subvar_end = ProcessVar(iterctx,
                                      &next_var[2], next_var[1]);

        /* Was there unbalanced paren for the inner expansion? */
        if (*subvar_end == '\0')
        {
            /* Despite unclosed parenthesis for the inner expansion,
             * the outer variable might close with a brace, or not. */
            next_var = s_end + FindDollarParen(s_end);
            /* s_end is already correct */
        }
        else                          /* inner variable processed correctly */
        {
            /* This variable depends on inner expansions. */
            dependencies++;
            /* We are sure (subvar_end+1) is not out of bounds. */
            s_end    = strchrnul(subvar_end + 1, closing_paren);
            next_var = subvar_end + 1 + FindDollarParen(subvar_end + 1);
        }
    }

    if (*s_end == '\0')
    {
        Log(LOG_LEVEL_ERR,
            "No closing '%c' found!", opposite(c)); /* TODO VERBOSE? */
        return s_end;
    }

    const size_t s_len = s_end - s;

    /* Change the variable name in order to mangle namespaces and scopes. */
    /* TODO VariableGet() and mangle only if it's iterable. Also add a wheel
     * again only if it's iterable. */
    MangleVarRefString(s, s_len);

    Wheel *new_wheel = WheelNew(s, s_len);

    /* If identical variable is already inserted, it means that it has
     * been seen before and has been inserted together with all
     * dependencies; skip. */
    /* It can happen if variables exist twice in a string, for example:
       "$(i) blah $(A[$(i)])" has i variable twice. */

    bool same_var_found = SeqLookup(iterctx->wheels, new_wheel,
                                    WheelCompareUnexpanded)  !=  NULL;
    if (same_var_found)
    {
        Log(LOG_LEVEL_DEBUG,
            "Skipped adding iteration wheel for already existing variable: %s",
            new_wheel->varname_unexp);
        WheelDestroy(new_wheel);
    }
    else
    {
        /* If this variable is dependent on other variables, we've already
         * appended the wheels of the dependencies during the recursive
         * calls. Or it happens and this is an independent variable. So
         * APPEND the wheel for this variable. */
        SeqAppend(iterctx->wheels, new_wheel);
        Log(LOG_LEVEL_DEBUG,
            "Added iteration wheel for variable: %s",
            new_wheel->varname_unexp);
    }

    assert(*s_end == closing_paren);
    return s_end;
}

/**
 *  @brief Fills up the wheels of the iterator according to the variables
 *         found in #s.
 *
 * @TODO TESTS
 *           ""
 *           "$(blah"
 *           "$(blah)"
 *           "$(blah))"
 *           "$(blah))$(blue)"
 *           "$(blah)$("
 *           "$(blah)$(blue)"
 *           "$(blah)$(blah)"
 *           "$(blah)1$(blue)"
 *           "0($blah)1$(blue)"
 *           "0($blah)1$(blue)2"
 */
void PromiseIteratorPrepare(PromiseIterator *iterctx,
//                             EvalContext *eval_ctx,
                            char *s)
{
    Log(LOG_LEVEL_DEBUG, "PromiseIteratorPrepare(\"%s\")", s);
    char *var_start = s + FindDollarParen(s);

    while (*var_start != '\0')
    {
        char paren_or_brace = var_start[1];
        var_start += 2;                                /* skip dollar-paren */

        assert(paren_or_brace == '(' || paren_or_brace == '{');

        char *var_end = ProcessVar(iterctx, var_start, paren_or_brace);

        var_start = var_end + 1 + FindDollarParen(var_end + 1);
    }
}

static bool WheelIncrement(PromiseIterator *iterctx, size_t i)
{
    Wheel *wheel = SeqAt(iterctx->wheels, i);

    wheel->iter_index++;
    if (wheel->values != NULL &&
        wheel->iter_index < SeqLength(wheel->values))
    {
        return true;
    }
    else
    {
        return false;
    }
}

static void IterListElementVariablePut(EvalContext *evalctx,
                                       const char *varname,
                                       DataType listtype, void *value)
{
    DataType t;

    switch (listtype)
    {
    case CF_DATA_TYPE_CONTAINER:   t = CF_DATA_TYPE_STRING; break;
    case CF_DATA_TYPE_STRING_LIST: t = CF_DATA_TYPE_STRING; break;
    case CF_DATA_TYPE_INT_LIST:    t = CF_DATA_TYPE_INT;    break;
    case CF_DATA_TYPE_REAL_LIST:   t = CF_DATA_TYPE_REAL;   break;
    default:
        t = CF_DATA_TYPE_NONE;                           /* silence warning */
        ProgrammingError("IterVariablePut() invalid type: %d",
                         listtype);
    }

    EvalContextVariablePutSpecial(evalctx, SPECIAL_SCOPE_THIS,
                                  varname, value,
                                  t, "source=promise_iteration");
}


/**
 * Get a variable value and type. Since we are in iteration context, the
 * scoped or namespaced variable names may be mangled, so we have to demangle
 * them before looking them up.
 *
 * @TODO what if looking them up must find the mangled reference?
 */
const void *IterVariableGet(const PromiseIterator *iterctx,
                            const EvalContext *evalctx,
                            const char *varname, DataType *type)
{
//    VarRef *ref = VarRefParseFromBundle(varname,
//                                        PromiseGetBundle(iterctx->pp));
    const Bundle *bundle = PromiseGetBundle(iterctx->pp);
    VarRef *ref =
        VarRefParseFromNamespaceAndScope(varname, bundle->ns, bundle->name,
                                         CF_MANGLED_NS, CF_MANGLED_SCOPE);
//    VarRef *ref = VarRefParse(varname);
    const void *value = EvalContextVariableGet(evalctx, ref, type);
    if (value == NULL)                                      /* TODO remove? */
    {
//       ProgrammingError("Couldn't find extracted variable: %s", varname);
    }

    VarRefDestroy(ref);
    return value;
}

static void SeqAppendContainerPrimitive(Seq *seq, const JsonElement *primitive)
{
    assert(JsonGetElementType(primitive) == JSON_ELEMENT_TYPE_PRIMITIVE);

    switch (JsonGetPrimitiveType(primitive))
    {
    case JSON_PRIMITIVE_TYPE_BOOL:
        SeqAppend(seq, (JsonPrimitiveGetAsBool(primitive) ?
                        xstrdup("true") : xstrdup("false")));
        break;
    case JSON_PRIMITIVE_TYPE_INTEGER:
    {
        char *str = StringFromLong(JsonPrimitiveGetAsInteger(primitive));
        SeqAppend(seq, str);
        break;
    }
    case JSON_PRIMITIVE_TYPE_REAL:
    {
        char *str = StringFromDouble(JsonPrimitiveGetAsReal(primitive));
        SeqAppend(seq, str);
        break;
    }
    case JSON_PRIMITIVE_TYPE_STRING:
        SeqAppend(seq, xstrdup(JsonPrimitiveGetAsString(primitive)));
        break;

    case JSON_PRIMITIVE_TYPE_NULL:
        break;
    }
}

Seq *ContainerToSeq(const JsonElement *container)
{
    Seq *seq = SeqNew(5, NULL);

    switch (JsonGetElementType(container))
    {
    case JSON_ELEMENT_TYPE_PRIMITIVE:
        SeqAppendContainerPrimitive(seq, container);
        break;

    case JSON_ELEMENT_TYPE_CONTAINER:
    {
        JsonIterator iter = JsonIteratorInit(container);
        const JsonElement *child;

        while ((child = JsonIteratorNextValue(&iter)) != NULL)
        {
            if (JsonGetElementType(child) == JSON_ELEMENT_TYPE_PRIMITIVE)
            {
                SeqAppendContainerPrimitive(seq, child);
            }
        }
        break;
    }
    }
    return seq;
}
/* TODO SeqFinalise() to save space? */

Seq *RlistToSeq(const Rlist *p)
{
    Seq *seq = SeqNew(5, NULL);

    const Rlist *rlist = p;
    while(rlist != NULL)
    {
        Rval val = rlist->val;
//TODO  what if val is int or float? Does it work as it is?
        SeqAppend(seq, val.item);
        rlist = rlist->next;
    }

    return seq;
}

Seq *IterableToSeq(const void *v, DataType t)
{
    switch (t)
    {
    case CF_DATA_TYPE_CONTAINER:
        return ContainerToSeq(v);
        break;
    case CF_DATA_TYPE_STRING_LIST:
    case CF_DATA_TYPE_INT_LIST:
    case CF_DATA_TYPE_REAL_LIST:
        /* All lists are stored as Rlist internally. */
        assert(DataTypeToRvalType(t) == RVAL_TYPE_LIST);
        return RlistToSeq(v);

    default:
        ProgrammingError("IterableToSeq() got non-iterable type: %d", t);
    }
}

/*
void IterVariablePut(const PromiseIterator *iterctx,
                     EvalContext *evalctx,
                     const char *varname, const void *value, DataType type)
{
    EvalContextVariablePutSpecial(evalctx, SPECIAL_SCOPE_THIS,
                                  varname, value,
                                  t, "source=promise_iteration");
}
*/
/*
 * TODO PromiseIteratorStart() that populates all the values.
 * OR even better, start it here if not already started.
 */

bool PromiseIteratorNext(PromiseIterator *iterctx, EvalContext *evalctx)
{
    size_t wheels_num = SeqLength(iterctx->wheels);

    size_t i;                     /* which wheel we're currently working on */

    if (iterctx->count > 0)         /* we've already iterated at least once */
    {
        if (wheels_num == 0)
        {
            /* nothing to iterate on, so get out after already having iterated
             * once. */
            return false;
        }

        /* Try incrementing the rightmost wheels first, i.e. the most
         * dependent variables. */
        i = wheels_num - 1;
        while (!WheelIncrement(iterctx, i))
        {
            if (i == 0)
            {
                return false;                 /* we've iterated over everything */
            }
            i--;                                /* increment the previous wheel */
        }

        /*
         * Alright, incrementing the wheel i was successful;  now:
         *
         * 1. add the new value of wheel i to EvalContext, (making sure that the
         *    variable already existed but the value gets replaced). This is
         *    basically the basic iteration step, just going to the next value of
         *    the iterable.
         */
        Wheel *wheel    = SeqAt(iterctx->wheels, i);
        void *new_value = SeqAt(wheel->values, wheel->iter_index);

        IterListElementVariablePut(
            evalctx, wheel->varname_exp, wheel->vartype, new_value);

    }
    else
    {
        /* If no iterations have happened yet, we initialise all wheels. */
        i = (size_t) -1;               /* increments to 0 in the loop start */

        Log(LOG_LEVEL_DEBUG,
            "STARTING ITERATION ENGINE, ENTERING WARP SPEED");
    }

    /* TODO Run it also once when initialising, to reset everything. */
    Log(LOG_LEVEL_DEBUG,
        "PromiseIteratorNext(): count=%zu wheels_num=%zu current_wheel=%zd",
        iterctx->count, wheels_num, (ssize_t) i);

    /*
     * For each of the subsequent wheels (if any, or all of them on the first
     * iteraration that i==(size_t)-1):
     *
     * 2. varname_exp = expand the variable name
     *    - if it's same with previous varname_exp, no need to Put()?
     * 3. values = VariableGet(varname_exp);
     * 4. if the value is an iterator
     *    (slist/container), set the wheel size.
     * 5. reset the wheel in order to re-iterate over all combinations.
     * 6. put varname_exp+first_value in the EvalContext
     *    (remove the previous wheel variable? Naaah...)
     */

    /* Buffer to store the expanded wheel variable name, for each wheel. */
    Buffer *tmpbuf = BufferNew();

    /* TODO I could replace this loop with PutAllWheelVariables() after I've
     * set all the wheel counters correct. */
    for (i = i + 1; i < wheels_num; i++)
    {
        Wheel *wheel = SeqAt(iterctx->wheels, i);
        BufferClear(tmpbuf);

        /* Reset wheel in order to re-iterate over all combinations. */
        wheel->iter_index = 0;

// TODO at this point, in the first iteration that we set up all wheels,
// const#dirsep has not been added to THIS scope. WHY?
// -->  As expected, because it doesn't depend on previous wheels.

        /* The wheel variable may depend on previous wheels, for example
         * "B_$(k)_$(v)" is dependent on variables "k" and "v", which are
         * wheels already set (at lower 'w' index). */
        const char *varname = ExpandScalar(evalctx,
                                           PromiseGetNamespace(iterctx->pp),
                                           "this",
                                           wheel->varname_unexp, tmpbuf);
        Log(LOG_LEVEL_DEBUG,
            "PromiseIteratorNext(): Expanded '%s' to '%s'",
            wheel->varname_unexp, varname);

        /* No need to do anything if the variable expands to the same value as
         * before (because possibly it doesn't have internal expansions). */
        /* NOT TRUE, we still have to reset the counter if it's an iterable
         * variable. */
        /* NOT TRUE, THE FRAME HAS BEEN POPPED! Todo add the zero-sized
         * (non-iterable) variables before iteration starts. */
        /* NULL if it's the first iteration. */
        if (wheel->varname_exp == NULL
            || strcmp(varname, wheel->varname_exp) != 0)
        {
            free(wheel->varname_exp);
            wheel->varname_exp = xstrdup(varname);

            /* After expanding the variable name, we have to lookup its value,
               and set the size of the wheel if it's an slist or container. */
            /* TODO what if after expansion the wheel is still dependent, i.e. the
             * expansion leads to some other expansion? Maybe we should loop here? */
            DataType t;
            const void *value = IterVariableGet(iterctx, evalctx, varname, &t);

            WheelValuesSeqDestroy(wheel);           /* free previous values */

            /* Set wheel values and size according to variable type. */
            if (IsIterable(t))
            {
                wheel->values = IterableToSeq(value, t);
                wheel->vartype = t;

                /* Put the first value of the iterable. */
                IterListElementVariablePut(evalctx, varname, t,
                                           SeqAt(wheel->values, 0));
            }
            else if (t != CF_DATA_TYPE_NONE && IsMangled(varname))
            {
                /* wheel->vartype = t; */
                /* wheel->values = SeqNew(1, NULL); */
                /* SeqAppend(wheel->values, &value); */
                EvalContextVariablePutSpecial(evalctx, SPECIAL_SCOPE_THIS,
                                              varname, value,
                                              t, "source=promise_iteration");
            }
            else
            {
                /* No need to VariablePut() the first element of this
                 * variable, since the variable is not an iterable and it
                 * already exists in the EvalContext. */
                assert(wheel->values == NULL);
            }
        }
        else
        {
            /* The variable name expanded to the same name, so the value is
             * the same and wheel->values is already correct. So if it's an
             * iterable, we VariablePut() the first element. */
            if (wheel->values != NULL)
            {
                /* Put the first value of the iterable. */
                IterListElementVariablePut(evalctx,
                                           wheel->varname_exp, wheel->vartype,
                                           SeqAt(wheel->values, 0));
            }
        }
    }

    BufferDestroy(tmpbuf);

    iterctx->count++;
    return true;
}


















static void RlistAppendContainerPrimitive(Rlist **list, const JsonElement *primitive)
{
    assert(JsonGetElementType(primitive) == JSON_ELEMENT_TYPE_PRIMITIVE);

    switch (JsonGetPrimitiveType(primitive))
    {
    case JSON_PRIMITIVE_TYPE_BOOL:
        RlistAppendScalar(list, JsonPrimitiveGetAsBool(primitive) ? "true" : "false");
        break;
    case JSON_PRIMITIVE_TYPE_INTEGER:
        {
            char *str = StringFromLong(JsonPrimitiveGetAsInteger(primitive));
            RlistAppendScalar(list, str);
            free(str);
        }
        break;
    case JSON_PRIMITIVE_TYPE_REAL:
        {
            char *str = StringFromDouble(JsonPrimitiveGetAsReal(primitive));
            RlistAppendScalar(list, str);
            free(str);
        }
        break;
    case JSON_PRIMITIVE_TYPE_STRING:
        RlistAppendScalar(list, JsonPrimitiveGetAsString(primitive));
        break;

    case JSON_PRIMITIVE_TYPE_NULL:
        break;
    }
}
Rlist *ContainerToRlist(const JsonElement *container)
{
    Rlist *list = NULL;

    switch (JsonGetElementType(container))
    {
    case JSON_ELEMENT_TYPE_PRIMITIVE:
        RlistAppendContainerPrimitive(&list, container);
        break;

    case JSON_ELEMENT_TYPE_CONTAINER:
        {
            JsonIterator iter = JsonIteratorInit(container);
            const JsonElement *child;

            while (NULL != (child = JsonIteratorNextValue(&iter)))
            {
                if (JsonGetElementType(child) == JSON_ELEMENT_TYPE_PRIMITIVE)
                {
                    RlistAppendContainerPrimitive(&list, child);
                }
            }
        }
        break;
    }

    return list;
}

#if 0
struct PromiseIterator_
{
    size_t idx;                                /* current iteration index */
    bool has_null_list;                        /* true if any list is empty */
    /* list of slist/container variables (of type CfAssoc) to iterate over. */
    Seq *vars;
    /* List of expanded values (Rlist) for each variable. The list can contain
     * NULLs if the slist has cf_null values. */
    Seq *var_states;
};

static Rlist *FirstRealEntry(Rlist *entry)
{
    while (entry && entry->val.item &&
           entry->val.type == RVAL_TYPE_SCALAR &&
           strcmp(entry->val.item, CF_NULL_VALUE) == 0)
    {
        entry = entry->next;
    }
    return entry;
}
static bool AppendIterationVariable(PromiseIterator *iter, CfAssoc *new_var)
{
    Rlist *state = RvalRlistValue(new_var->rval);
    // move to the first non-null value
    state = FirstRealEntry(state);
    SeqAppend(iter->vars, new_var);
    SeqAppend(iter->var_states, state);
    return state != NULL;
}

PromiseIterator *PromiseIteratorNew(EvalContext *ctx, const Promise *pp,
                                    const Rlist *lists,
                                    const Rlist *containers)
{
    int lists_len      = RlistLen(lists);
    int containers_len = RlistLen(containers);
    PromiseIterator *iter = xcalloc(1, sizeof(*iter));

    iter->idx           = 0;
    iter->has_null_list = false;
    iter->vars       = SeqNew(lists_len + containers_len, DeleteAssoc);
    iter->var_states = SeqNew(lists_len + containers_len, NULL);

    for (const Rlist *rp = lists; rp != NULL; rp = rp->next)
    {
        VarRef *ref = VarRefParseFromBundle(RlistScalarValue(rp),
                                            PromiseGetBundle(pp));
        DataType dtype;
        const void *value = EvalContextVariableGet(ctx, ref, &dtype);
        if (!value)
        {
            Log(LOG_LEVEL_ERR,
                "Couldn't locate variable '%s' apparently in '%s'",
                RlistScalarValue(rp), PromiseGetBundle(pp)->name);
            VarRefDestroy(ref);
            continue;
        }

        VarRefDestroy(ref);

        assert(DataTypeToRvalType(dtype) == RVAL_TYPE_LIST);

        /* Could use NewAssoc() but I don't for consistency with code next. */
        CfAssoc *new_var = xmalloc(sizeof(CfAssoc));
        new_var->lval = xstrdup(RlistScalarValue(rp));
        new_var->rval = RvalNew(value, RVAL_TYPE_LIST);
        new_var->dtype = dtype;

        iter->has_null_list |= !AppendIterationVariable(iter, new_var);
    }

    for (const Rlist *rp = containers; rp; rp = rp->next)
    {
        VarRef *ref = VarRefParseFromBundle(RlistScalarValue(rp),
                                            PromiseGetBundle(pp));
        DataType dtype;
        const JsonElement *value = EvalContextVariableGet(ctx, ref, &dtype);
        if (!value)
        {
            Log(LOG_LEVEL_ERR,
                "Couldn't locate variable '%s' apparently in '%s'",
                RlistScalarValue(rp), PromiseGetBundle(pp)->name);
            VarRefDestroy(ref);
            continue;
        }

        VarRefDestroy(ref);

        assert(dtype == CF_DATA_TYPE_CONTAINER);

        /* Mimics NewAssoc() but bypassing extra copying of ->rval: */
        CfAssoc *new_var = xmalloc(sizeof(CfAssoc));
        new_var->lval = xstrdup(RlistScalarValue(rp));
        new_var->rval = (Rval) { ContainerToRlist(value), RVAL_TYPE_LIST };
        new_var->dtype = CF_DATA_TYPE_STRING_LIST;

        iter->has_null_list |= !AppendIterationVariable(iter, new_var);
    }

    Log(LOG_LEVEL_DEBUG,
        "Start iterating over %3d slists and %3d containers,"
        " for promise:  '%s'",
        lists_len, containers_len, pp->promiser);

    return iter;
}

void PromiseIteratorDestroy(PromiseIterator *iter)
{
    Log(LOG_LEVEL_DEBUG, "Completed %5zu iterations over the promise",
        iter->idx + 1);

    if (iter)
    {
        for (size_t i = 0; i < SeqLength(iter->vars); i++)
        {
            CfAssoc *var = SeqAt(iter->vars, i);
            void *state = SeqAt(iter->var_states, i);

            if (var->rval.type == RVAL_TYPE_CONTAINER)
            {
                free(state);
            }
        }

        SeqDestroy(iter->var_states);
        SeqDestroy(iter->vars);
        free(iter);
    }
}

bool PromiseIteratorHasNullIterators(const PromiseIterator *iter)
{
    return iter->has_null_list;
}

/*****************************************************************************/

static bool VariableStateIncrement(PromiseIterator *iter, size_t index)
{
    assert(index < SeqLength(iter->var_states));

    CfAssoc *var = SeqAt(iter->vars, index);

    switch (var->rval.type)
    {
    case RVAL_TYPE_LIST:
        {
            Rlist *state = SeqAt(iter->var_states, index);
            assert(state);
            // find the next valid value, return false if there is none
            state = FirstRealEntry(state->next);
            SeqSet(iter->var_states, index, state);
            return state != NULL;
        }
        break;

    default:
        ProgrammingError("Unhandled case in switch");
    }

    return false;
}

static bool VariableStateReset(PromiseIterator *iter, size_t index)
{
    assert(index < SeqLength(iter->var_states));

    CfAssoc *var = SeqAt(iter->vars, index);

    switch (var->rval.type)
    {
    case RVAL_TYPE_LIST:
        {
            Rlist *state = RvalRlistValue(var->rval);
            // find the first valid value, return false if there is none
            state = FirstRealEntry(state);
            SeqSet(iter->var_states, index, state);
            return state != NULL;
        }
        break;

    default:
        ProgrammingError("Unhandled case in switch");
    }

    return false;
}

static bool VariableStateHasMore(const PromiseIterator *iter, size_t index)
{
    CfAssoc *var = SeqAt(iter->vars, index);
    switch (var->rval.type)
    {
    case RVAL_TYPE_LIST:
        {
            const Rlist *state = SeqAt(iter->var_states, index);
            assert(state != NULL);

            return (state->next != NULL);
        }

    default:
        ProgrammingError("Variable is not an slist (variable type: %d)",
                         var->rval.type);
    }

    return false;
}

static bool IncrementIterationContextInternal(PromiseIterator *iter,
                                              size_t wheel_idx)
{
    /* How many wheels (slists/containers) we have. */
    size_t wheel_max = SeqLength(iter->vars);

    if (wheel_idx == wheel_max)
    {
        return false;
    }

    assert(wheel_idx < wheel_max);

    if (VariableStateHasMore(iter, wheel_idx))
    {
        // printf("%*c\n", (int) wheel_idx + 1, 'I');

        /* Update the current wheel, i.e. get the next slist item. */
        return VariableStateIncrement(iter, wheel_idx);
    }
    else                          /* this wheel has come to full revolution */
    {
        /* Try to increase the next wheel. */
        if (IncrementIterationContextInternal(iter, wheel_idx + 1))
        {
            // printf("%*c\n", (int) wheel_idx + 1, 'R');

            /* We successfully increased one of the next wheels, so reset this
             * one to iterate over all possible states. */
            return VariableStateReset(iter, wheel_idx);
        }
        else                      /* there were no more wheels to increment */
        {
            return false;
        }
    }
}

bool PromiseIteratorNext(PromiseIterator *iter_ctx)
{
    if (IncrementIterationContextInternal(iter_ctx, 0))
    {
        iter_ctx->idx++;
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * Put the newly iterated variable value in the EvalContext.
 */
void PromiseIteratorUpdateVariables(EvalContext *ctx, const PromiseIterator *iter)
{
    for (size_t i = 0; i < SeqLength(iter->vars); i++)
    {
        DataType t;
        const CfAssoc *iter_var = SeqAt(iter->vars, i);
        const Rlist   *state    = SeqAt(iter->var_states, i);

        if (!state || state->val.type == RVAL_TYPE_FNCALL)
        {
            continue;
        }

        switch (iter_var->dtype)
        {
        case CF_DATA_TYPE_STRING_LIST: t = CF_DATA_TYPE_STRING; break;
        case CF_DATA_TYPE_INT_LIST:    t = CF_DATA_TYPE_INT;    break;
        case CF_DATA_TYPE_REAL_LIST:   t = CF_DATA_TYPE_REAL;   break;
        default:
            t = CF_DATA_TYPE_NONE;                       /* silence warning */
            ProgrammingError("CfAssoc contains invalid type: %d",
                             iter_var->dtype);
        }

        assert(state->val.type == RVAL_TYPE_SCALAR);

        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_THIS,
                                      iter_var->lval, RlistScalarValue(state),
                                      t, "source=promise");
    }
}

size_t PromiseIteratorIndex(const PromiseIterator *iter_ctx)
{
    return iter_ctx->idx;
}
#endif
