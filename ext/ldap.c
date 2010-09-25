#include <ruby.h>
#ifndef RSTRING_PTR
#define RSTRING_PTR(str) RSTRING(str)->ptr
#endif
#ifndef RSTRING_LEN
#define RSTRING_LEN(str) RSTRING(str)->len
#endif

#include <ldap.h>
#include <lber.h>
#include <stdlib.h>

static VALUE cLDAP;
static VALUE cLDAP_Message;
static VALUE eLDAP;

typedef struct {
	LDAP *ld;
	char connection;
} RLDAP_WRAP;

typedef struct {
	LDAP *ld;
	LDAPMessage *mesg;
	char freed;
} RLDAP_MSG_WRAP;

static RLDAP_WRAP *get_wrapper(VALUE obj)
{
	RLDAP_WRAP *wrapper;
	Data_Get_Struct(obj, RLDAP_WRAP, wrapper);
	return wrapper;
}

static RLDAP_MSG_WRAP *get_msg_wrapper(VALUE obj)
{
	RLDAP_MSG_WRAP* wrapper;
	Data_Get_Struct(obj, RLDAP_MSG_WRAP, wrapper);
	return wrapper;
}

static void free_wrapper(RLDAP_WRAP *wrapper)
{
	ldap_memfree(wrapper->ld);
	xfree(wrapper);
}

static void free_msg_wrapper(RLDAP_MSG_WRAP *wrapper)
{
	if (wrapper->freed == Qfalse)
		ldap_msgfree(wrapper->mesg);
	xfree(wrapper);
}

static void rldap_raise(int errno)
{
	VALUE e = rb_exc_new2(eLDAP, ldap_err2string(errno));
	rb_iv_set(e, "@errno", INT2FIX(errno));
	rb_exc_raise(e);
}

static VALUE ldapmessage2obj(LDAP *ld, LDAPMessage *msg);

/* class LDAP */

static VALUE rldap_err2string(VALUE klass, VALUE rerrno)
{
	int errno;
	errno = FIX2INT(rerrno);
	return rb_str_new2(ldap_err2string(errno));
}

static VALUE rldap_alloc(VALUE klass)
{
	RLDAP_WRAP *wrapper;
	VALUE obj;

	obj = Data_Make_Struct(klass, RLDAP_WRAP, 0, free_wrapper, wrapper);
	wrapper->connection = Qfalse;

	return obj;
}

static VALUE rldap_initialize(int argc, VALUE *argv, VALUE obj)
{
	VALUE rhost, rport;
	char *host;
	int port;
	RLDAP_WRAP *wrapper;

	rb_scan_args(argc, argv, "11", &rhost, &rport);

	if (NIL_P(rport))
		rport = INT2FIX(LDAP_PORT);

	wrapper = get_wrapper(obj);
	host = StringValuePtr(rhost);
	port = FIX2INT(rport);

	wrapper->ld = (LDAP *)ldap_init(host, port);

	return obj;
}

static VALUE rldap_start_tls(VALUE obj)
{
	RLDAP_WRAP *wrapper;
	int retval;

	wrapper = get_wrapper(obj);
	retval = ldap_start_tls_s(wrapper->ld, NULL, NULL);
	if (retval == LDAP_SUCCESS)
		return Qtrue;
	else
		rldap_raise(retval);
}

static VALUE rldap_search(int argc, VALUE *argv, VALUE obj)
{
	RLDAP_WRAP *wrapper;
	char *base, *filter;
	int retval, count, i, scope;
	LDAPMessage *res, *msg;
	VALUE ary, rbase, rfilter, rscope;
	ID iscope;
	
	rb_scan_args(argc, argv, "21", &rbase, &rfilter, &rscope);

	switch(TYPE(rscope)) {
		case T_NIL:
			scope = LDAP_SCOPE_SUBTREE;
			break;
		case T_FIXNUM:
			scope = FIX2INT(rscope);
			break;
		case T_SYMBOL:
		case T_STRING:
			iscope = rb_to_id(rscope);
			if (iscope == rb_intern("subtree"))
				scope = LDAP_SCOPE_SUBTREE;
			if (iscope == rb_intern("base"))
				scope = LDAP_SCOPE_BASE;
			if (iscope == rb_intern("one"))
				scope = LDAP_SCOPE_ONE;
			break;
		default:
			rb_raise(rb_eTypeError, "not a valid scope");
			break;
	}
	
	wrapper = get_wrapper(obj);
	base = StringValuePtr(rbase);
	filter = StringValuePtr(rfilter);

	retval = ldap_search_ext_s(wrapper->ld, base, scope, filter, NULL, 0, NULL, NULL, NULL, 0, &res);

	if (retval != LDAP_SUCCESS)
		rldap_raise(retval);

	count = ldap_count_entries(wrapper->ld, res);
	
	if (count == -1) {
		int errno;
		ldap_get_option(wrapper->ld, LDAP_OPT_RESULT_CODE, &errno);
		rldap_raise(errno);
	}
	
	ary = rb_ary_new2((long)count);
	
	msg = ldap_first_entry(wrapper->ld, res);
	
	for (i=0; i<count; i++) {
		rb_ary_store(ary, (long)i, ldapmessage2obj(wrapper->ld, msg));
		msg = ldap_next_entry(wrapper->ld, msg);
	}
	
	return ary;
}

static VALUE rldap_set_option(VALUE obj, VALUE roption, VALUE rvalue)
{
	RLDAP_WRAP *wrapper;
	int retval;
	int option;
	int ival;
	char *sval;
	void *val;
	
	wrapper = get_wrapper(obj);
	option = FIX2INT(roption);
	
	if (TYPE(rvalue) == T_STRING) {
		sval = StringValuePtr(rvalue);
		val = &sval;
	} else {
		ival = FIX2INT(rvalue);
		val = &ival;
	}
	
	retval = ldap_set_option(wrapper->ld, option, val);
	
	if (retval == LDAP_OPT_SUCCESS)
		return Qtrue;
	else
		return Qfalse;
}

static VALUE rldap_errno(VALUE obj)
{
	RLDAP_WRAP *wrapper;
	int errno;
	
	wrapper = get_wrapper(obj);
	
	ldap_get_option(wrapper->ld, LDAP_OPT_RESULT_CODE, &errno);
	
	return INT2NUM(errno);
}

int rldap_errno_c(VALUE obj)
{
	RLDAP_WRAP *wrapper;
	int errno;

	wrapper = get_wrapper(obj);
	ldap_get_option(wrapper->ld, LDAP_OPT_RESULT_CODE, &errno);
	return errno;
}


/* class LDAP::Message */

static VALUE ldapmessage2obj(LDAP* ld, LDAPMessage* msg)
{
	VALUE obj;
	RLDAP_MSG_WRAP *wrapper;

	obj = Data_Make_Struct(cLDAP_Message, RLDAP_MSG_WRAP, 0, free_msg_wrapper, wrapper);
	wrapper->mesg = msg;
	wrapper->freed = Qfalse;
	wrapper->ld = ld;

	return obj;
}

static VALUE rldap_msg_dn(VALUE obj)
{
	RLDAP_MSG_WRAP *wrapper;
	char *dn;
	VALUE rdn;
	
	wrapper = get_msg_wrapper(obj);
	
	dn = ldap_get_dn(wrapper->ld, wrapper->mesg);
	rdn = rb_str_new2(dn);
	ldap_memfree(dn);
	
	return rdn;
}

static VALUE rldap_msg_get_val(VALUE obj, VALUE key)
{
	RLDAP_MSG_WRAP *wrapper;
	char *attr, *strval;
	BerValue **values;
	VALUE ary, str;
	int i, length;
	BerValue *value;
	
	wrapper = get_msg_wrapper(obj);
	attr = StringValuePtr(key);
	
	values = ldap_get_values_len(wrapper->ld, wrapper->mesg, attr);

	if (values == NULL) {
		rldap_raise(rldap_errno_c(obj));
	}
	
	ary = rb_ary_new();
	length = ldap_count_values_len(values);
	
	for (i=0; i<length; i++) {
		value = values[i];
		str = rb_str_new(value->bv_val, value->bv_len);
		rb_ary_push(ary, str);
	}
	
	ldap_value_free_len(values);
	
	return ary;
}

static VALUE rldap_msg_keys(VALUE obj)
{
	RLDAP_MSG_WRAP *wrapper;
	char *attr;
	BerElement *ber;
	VALUE ary;

	ary = rb_iv_get(obj, "@keys");

	if (ary != Qnil)
		return ary;

	wrapper = get_msg_wrapper(obj);
	ary = rb_ary_new();

	attr = ldap_first_attribute(wrapper->ld, wrapper->mesg, &ber);
	do {
		rb_ary_push(ary, rb_str_new2(attr));
		ldap_memfree(attr);
	} while (attr = ldap_next_attribute(wrapper->ld, wrapper->mesg, ber));

	ber_free(ber, 0);

	rb_iv_set(obj, "@keys", ary);

	return ary;
}


void Init_ldap()
{
	cLDAP = rb_define_class("LDAP", rb_cObject);
	cLDAP_Message = rb_define_class_under(cLDAP, "Message", rb_cObject);
	eLDAP = rb_define_class_under(cLDAP, "Error", rb_eStandardError);

	rb_define_alloc_func(cLDAP, rldap_alloc);
	rb_define_singleton_method(cLDAP, "err2string", rldap_err2string, 1);
	rb_define_method(cLDAP, "initialize", rldap_initialize, -1);
	rb_define_method(cLDAP, "start_tls", rldap_start_tls, 0);
	rb_define_method(cLDAP, "search", rldap_search, -1);
	rb_define_method(cLDAP, "set_option", rldap_set_option, 2);
	rb_define_method(cLDAP, "errno", rldap_errno, 0);
	
	rb_define_method(cLDAP_Message, "dn", rldap_msg_dn, 0);
	rb_define_method(cLDAP_Message, "[]", rldap_msg_get_val, 1);
	rb_define_method(cLDAP_Message, "keys", rldap_msg_keys, 0);
	
	rb_require("ldap/helpers");
	
	#include "constants.h"
}
