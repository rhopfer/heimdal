/*
 * Copyright (c) 1997 - 2005 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "gssapi_locl.h"

RCSID("$Id$");

/*
 * copy the addresses from `input_chan_bindings' (if any) to
 * the auth context `ac'
 */

static OM_uint32
set_addresses (krb5_auth_context ac,
	       const gss_channel_bindings_t input_chan_bindings)	       
{
    /* Port numbers are expected to be in application_data.value, 
     * initator's port first */ 

    krb5_address initiator_addr, acceptor_addr;
    krb5_error_code kret;
       
    if (input_chan_bindings == GSS_C_NO_CHANNEL_BINDINGS
	|| input_chan_bindings->application_data.length !=
	2 * sizeof(ac->local_port))
	return 0;

    memset(&initiator_addr, 0, sizeof(initiator_addr));
    memset(&acceptor_addr, 0, sizeof(acceptor_addr));
       
    ac->local_port =
	*(int16_t *) input_chan_bindings->application_data.value;
       
    ac->remote_port =
	*((int16_t *) input_chan_bindings->application_data.value + 1);
       
    kret = gss_address_to_krb5addr(input_chan_bindings->acceptor_addrtype,
				   &input_chan_bindings->acceptor_address,
				   ac->remote_port,
				   &acceptor_addr);
    if (kret)
	return kret;
           
    kret = gss_address_to_krb5addr(input_chan_bindings->initiator_addrtype,
				   &input_chan_bindings->initiator_address,
				   ac->local_port,
				   &initiator_addr);
    if (kret) {
	krb5_free_address (gssapi_krb5_context, &acceptor_addr);
	return kret;
    }
       
    kret = krb5_auth_con_setaddrs(gssapi_krb5_context,
				  ac,
				  &initiator_addr,  /* local address */
				  &acceptor_addr);  /* remote address */
       
    krb5_free_address (gssapi_krb5_context, &initiator_addr);
    krb5_free_address (gssapi_krb5_context, &acceptor_addr);
       
#if 0
    free(input_chan_bindings->application_data.value);
    input_chan_bindings->application_data.value = NULL;
    input_chan_bindings->application_data.length = 0;
#endif

    return kret;
}

/*
 * handle delegated creds in init-sec-context
 */

static void
do_delegation (krb5_auth_context ac,
	       krb5_ccache ccache,
	       krb5_creds *cred,
	       const gss_name_t target_name,
	       krb5_data *fwd_data,
	       int *flags)
{
    krb5_creds creds;
    krb5_kdc_flags fwd_flags;
    krb5_error_code kret;
       
    memset (&creds, 0, sizeof(creds));
    krb5_data_zero (fwd_data);
       
    kret = krb5_cc_get_principal(gssapi_krb5_context, ccache, &creds.client);
    if (kret) 
	goto out;
       
    kret = krb5_build_principal(gssapi_krb5_context,
				&creds.server,
				strlen(creds.client->realm),
				creds.client->realm,
				KRB5_TGS_NAME,
				creds.client->realm,
				NULL);
    if (kret)
	goto out; 
       
    creds.times.endtime = 0;
       
    fwd_flags.i = 0;
    fwd_flags.b.forwarded = 1;
    fwd_flags.b.forwardable = 1;
       
    if ( /*target_name->name.name_type != KRB5_NT_SRV_HST ||*/
	target_name->name.name_string.len < 2) 
	goto out;
       
    kret = krb5_get_forwarded_creds(gssapi_krb5_context,
				    ac,
				    ccache,
				    fwd_flags.i,
				    target_name->name.name_string.val[1],
				    &creds,
				    fwd_data);
       
 out:
    if (kret)
	*flags &= ~GSS_C_DELEG_FLAG;
    else
	*flags |= GSS_C_DELEG_FLAG;
       
    if (creds.client)
	krb5_free_principal(gssapi_krb5_context, creds.client);
    if (creds.server)
	krb5_free_principal(gssapi_krb5_context, creds.server);
}

/*
 * first stage of init-sec-context
 */

static OM_uint32
init_auth
(OM_uint32 * minor_status,
 const gss_cred_id_t initiator_cred_handle,
 gss_ctx_id_t * context_handle,
 const gss_name_t target_name,
 const gss_OID mech_type,
 OM_uint32 req_flags,
 OM_uint32 time_req,
 const gss_channel_bindings_t input_chan_bindings,
 const gss_buffer_t input_token,
 gss_OID * actual_mech_type,
 gss_buffer_t output_token,
 OM_uint32 * ret_flags,
 OM_uint32 * time_rec
    )
{
    OM_uint32 ret = GSS_S_FAILURE;
    krb5_error_code kret;
    krb5_flags ap_options;
    krb5_creds this_cred, *cred = NULL;
    krb5_data outbuf;
    krb5_ccache ccache = NULL;
    uint32_t flags;
    krb5_data authenticator;
    Checksum cksum;
    krb5_enctype enctype;
    krb5_data fwd_data;
    OM_uint32 lifetime_rec;

    krb5_data_zero(&outbuf);
    krb5_data_zero(&fwd_data);

    *minor_status = 0;

    *context_handle = malloc(sizeof(**context_handle));
    if (*context_handle == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }

    (*context_handle)->auth_context = NULL;
    (*context_handle)->source       = NULL;
    (*context_handle)->target       = NULL;
    (*context_handle)->flags        = 0;
    (*context_handle)->more_flags   = 0;
    (*context_handle)->ticket       = NULL;
    (*context_handle)->lifetime     = GSS_C_INDEFINITE;
    (*context_handle)->order	    = NULL;
    HEIMDAL_MUTEX_init(&(*context_handle)->ctx_id_mutex);

    kret = krb5_auth_con_init (gssapi_krb5_context,
			       &(*context_handle)->auth_context);
    if (kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }

    kret = set_addresses ((*context_handle)->auth_context,
			  input_chan_bindings);
    if (kret) {
	*minor_status = kret;
	ret = GSS_S_BAD_BINDINGS;
	goto failure;
    }

    krb5_auth_con_addflags(gssapi_krb5_context,
			   (*context_handle)->auth_context,
			   KRB5_AUTH_CONTEXT_DO_SEQUENCE |
			   KRB5_AUTH_CONTEXT_CLEAR_FORWARDED_CRED,
			   NULL);

    if (actual_mech_type)
	*actual_mech_type = GSS_KRB5_MECHANISM;

    if (initiator_cred_handle == GSS_C_NO_CREDENTIAL) {
	kret = krb5_cc_default (gssapi_krb5_context, &ccache);
	if (kret) {
	    gssapi_krb5_set_error_string ();
	    *minor_status = kret;
	    ret = GSS_S_FAILURE;
	    goto failure;
	}
    } else
	ccache = initiator_cred_handle->ccache;

    kret = krb5_cc_get_principal (gssapi_krb5_context,
				  ccache,
				  &(*context_handle)->source);
    if (kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }

    kret = krb5_copy_principal (gssapi_krb5_context,
				target_name,
				&(*context_handle)->target);
    if (kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }

    ret = _gss_DES3_get_mic_compat(minor_status, *context_handle);
    if (ret)
	goto failure;


    memset(&this_cred, 0, sizeof(this_cred));
    this_cred.client          = (*context_handle)->source;
    this_cred.server          = (*context_handle)->target;
    if (time_req && time_req != GSS_C_INDEFINITE) {
	krb5_timestamp ts;

	krb5_timeofday (gssapi_krb5_context, &ts);
	this_cred.times.endtime = ts + time_req;
    } else
	this_cred.times.endtime   = 0;
    this_cred.session.keytype = KEYTYPE_NULL;

    kret = krb5_get_credentials (gssapi_krb5_context,
				 0,
				 ccache,
				 &this_cred,
				 &cred);

    if (kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }

    (*context_handle)->lifetime = cred->times.endtime;

    ret = gssapi_lifetime_left(minor_status,
			       (*context_handle)->lifetime,
			       &lifetime_rec);
    if (ret) {
	goto failure;
    }

    if (lifetime_rec == 0) {
	*minor_status = 0;
	ret = GSS_S_CONTEXT_EXPIRED;
	goto failure;
    }

    krb5_auth_con_setkey(gssapi_krb5_context, 
			 (*context_handle)->auth_context, 
			 &cred->session);

    kret = krb5_auth_con_generatelocalsubkey(gssapi_krb5_context, 
					     (*context_handle)->auth_context,
					     &cred->session);
    if(kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }
    
    /* 
     * If the credential doesn't have ok-as-delegate, check what local
     * policy say about ok-as-delegate, default is FALSE that makes
     * code ignore the KDC setting and follow what the application
     * requested. If its TRUE, strip of the GSS_C_DELEG_FLAG if the
     * KDC doesn't set ok-as-delegate.
     */
    if (!cred->flags.b.ok_as_delegate) {
	krb5_boolean delegate;
    
	krb5_appdefault_boolean(gssapi_krb5_context,
				"gssapi", target_name->realm,
				"ok-as-delegate", FALSE, &delegate);
	if (delegate)
	    req_flags &= ~GSS_C_DELEG_FLAG;
    }

    flags = 0;
    ap_options = 0;
    if (req_flags & GSS_C_DELEG_FLAG)
	do_delegation ((*context_handle)->auth_context,
		       ccache, cred, target_name, &fwd_data, &flags);
    
    if (req_flags & GSS_C_MUTUAL_FLAG) {
	flags |= GSS_C_MUTUAL_FLAG;
	ap_options |= AP_OPTS_MUTUAL_REQUIRED;
    }
    
    if (req_flags & GSS_C_REPLAY_FLAG)
	flags |= GSS_C_REPLAY_FLAG;
    if (req_flags & GSS_C_SEQUENCE_FLAG)
	flags |= GSS_C_SEQUENCE_FLAG;
    if (req_flags & GSS_C_ANON_FLAG)
	;                               /* XXX */
    flags |= GSS_C_CONF_FLAG;
    flags |= GSS_C_INTEG_FLAG;
    flags |= GSS_C_TRANS_FLAG;
    
    if (ret_flags)
	*ret_flags = flags;
    (*context_handle)->flags = flags;
    (*context_handle)->more_flags |= LOCAL;
    
    ret = gssapi_krb5_create_8003_checksum (minor_status,
					    input_chan_bindings,
					    flags,
					    &fwd_data,
					    &cksum);
    krb5_data_free (&fwd_data);
    if (ret)
	goto failure;

    enctype = (*context_handle)->auth_context->keyblock->keytype;

    kret = krb5_build_authenticator (gssapi_krb5_context,
				     (*context_handle)->auth_context,
				     enctype,
				     cred,
				     &cksum,
				     NULL,
				     &authenticator,
				     KRB5_KU_AP_REQ_AUTH);

    if (kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }

    kret = krb5_build_ap_req (gssapi_krb5_context,
			      enctype,
			      cred,
			      ap_options,
			      authenticator,
			      &outbuf);

    if (kret) {
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	ret = GSS_S_FAILURE;
	goto failure;
    }

    ret = gssapi_krb5_encapsulate (minor_status, &outbuf, output_token,
				   (u_char *)"\x01\x00", GSS_KRB5_MECHANISM);
    if (ret)
	goto failure;

    krb5_data_free (&outbuf);
    krb5_free_creds(gssapi_krb5_context, cred);
    free_Checksum(&cksum);
    if (initiator_cred_handle == GSS_C_NO_CREDENTIAL)
	krb5_cc_close(gssapi_krb5_context, ccache);

    if (flags & GSS_C_MUTUAL_FLAG) {
	return GSS_S_CONTINUE_NEEDED;
    } else {
	int32_t seq_number;
	int is_cfx = 0;
	
	krb5_auth_getremoteseqnumber (gssapi_krb5_context,
				      (*context_handle)->auth_context,
				      &seq_number);

	gsskrb5_is_cfx(*context_handle, &is_cfx);

	ret = _gssapi_msg_order_create(minor_status,
				       &(*context_handle)->order,
				       _gssapi_msg_order_f(flags),
				       seq_number, 0, is_cfx);
	if (ret)
	    goto failure;

	if (time_rec)
	    *time_rec = lifetime_rec;

	(*context_handle)->more_flags |= OPEN;
	return GSS_S_COMPLETE;
    }

 failure:
    krb5_auth_con_free (gssapi_krb5_context,
			(*context_handle)->auth_context);
    krb5_data_free (&outbuf);
    if(cred)
	krb5_free_creds(gssapi_krb5_context, cred);
    if (ccache && initiator_cred_handle == GSS_C_NO_CREDENTIAL)
	krb5_cc_close(gssapi_krb5_context, ccache);
    if((*context_handle)->source)
	krb5_free_principal (gssapi_krb5_context,
			     (*context_handle)->source);
    if((*context_handle)->target)
	krb5_free_principal (gssapi_krb5_context,
			     (*context_handle)->target);
    if((*context_handle)->order)
	_gssapi_msg_order_destroy(&(*context_handle)->order);
    HEIMDAL_MUTEX_destroy(&(*context_handle)->ctx_id_mutex);
    free (*context_handle);
    *context_handle = GSS_C_NO_CONTEXT;
    return ret;
}

static OM_uint32
repl_mutual
           (OM_uint32 * minor_status,
            const gss_cred_id_t initiator_cred_handle,
            gss_ctx_id_t * context_handle,
            const gss_name_t target_name,
            const gss_OID mech_type,
            OM_uint32 req_flags,
            OM_uint32 time_req,
            const gss_channel_bindings_t input_chan_bindings,
            const gss_buffer_t input_token,
            gss_OID * actual_mech_type,
            gss_buffer_t output_token,
            OM_uint32 * ret_flags,
            OM_uint32 * time_rec
           )
{
    OM_uint32 ret, seq_number;
    krb5_error_code kret;
    krb5_data indata;
    krb5_ap_rep_enc_part *repl;
    int is_cfx = 0;

    output_token->length = 0;
    output_token->value = NULL;

    HEIMDAL_MUTEX_lock(&(*context_handle)->ctx_id_mutex);

    if (actual_mech_type)
	*actual_mech_type = GSS_KRB5_MECHANISM;

    ret = gssapi_krb5_decapsulate (minor_status, input_token, &indata,
				   "\x02\x00", GSS_KRB5_MECHANISM);
    if (ret) {
	HEIMDAL_MUTEX_unlock(&(*context_handle)->ctx_id_mutex);
	/* XXX - Handle AP_ERROR */
	return ret;
    }
    
    kret = krb5_rd_rep (gssapi_krb5_context,
			(*context_handle)->auth_context,
			&indata,
			&repl);
    if (kret) {
	HEIMDAL_MUTEX_unlock(&(*context_handle)->ctx_id_mutex);
	gssapi_krb5_set_error_string ();
	*minor_status = kret;
	return GSS_S_FAILURE;
    }
    krb5_free_ap_rep_enc_part (gssapi_krb5_context,
			       repl);
    
    krb5_auth_getremoteseqnumber (gssapi_krb5_context,
				  (*context_handle)->auth_context,
				  &seq_number);

    gsskrb5_is_cfx(*context_handle, &is_cfx);

    ret = _gssapi_msg_order_create(minor_status,
				   &(*context_handle)->order,
				   _gssapi_msg_order_f((*context_handle)->flags),
				   seq_number, 0, is_cfx);
    if (ret) {
	HEIMDAL_MUTEX_unlock(&(*context_handle)->ctx_id_mutex);
	return ret;
    }
	
    (*context_handle)->more_flags |= OPEN;

    *minor_status = 0;
    if (time_rec) {
	ret = gssapi_lifetime_left(minor_status,
				   (*context_handle)->lifetime,
				   time_rec);
    } else {
	ret = GSS_S_COMPLETE;
    }
    if (ret_flags)
	*ret_flags = (*context_handle)->flags;
    HEIMDAL_MUTEX_unlock(&(*context_handle)->ctx_id_mutex);

    return ret;
}

static OM_uint32
gsskrb5_init_sec_context
           (OM_uint32 * minor_status,
            const gss_cred_id_t initiator_cred_handle,
            gss_ctx_id_t * context_handle,
            const gss_name_t target_name,
            const gss_OID mech_type,
            OM_uint32 req_flags,
            OM_uint32 time_req,
            const gss_channel_bindings_t input_chan_bindings,
            const gss_buffer_t input_token,
            gss_OID * actual_mech_type,
            gss_buffer_t output_token,
            OM_uint32 * ret_flags,
            OM_uint32 * time_rec
	   )
{
    if (input_token == GSS_C_NO_BUFFER || input_token->length == 0)
	return init_auth (minor_status,
			  initiator_cred_handle,
			  context_handle,
			  target_name,
			  mech_type,
			  req_flags,
			  time_req,
			  input_chan_bindings,
			  input_token,
			  actual_mech_type,
			  output_token,
			  ret_flags,
			  time_rec);
    else
	return repl_mutual(minor_status,
			   initiator_cred_handle,
			   context_handle,
			   target_name,
			   mech_type,
			   req_flags,
			   time_req,
			   input_chan_bindings,
			   input_token,
			   actual_mech_type,
			   output_token,
			   ret_flags,
			   time_rec);
}

static OM_uint32
spnego_reply
           (OM_uint32 * minor_status,
            const gss_cred_id_t initiator_cred_handle,
            gss_ctx_id_t * context_handle,
            const gss_name_t target_name,
            const gss_OID mech_type,
            OM_uint32 req_flags,
            OM_uint32 time_req,
            const gss_channel_bindings_t input_chan_bindings,
            const gss_buffer_t input_token,
            gss_OID * actual_mech_type,
            gss_buffer_t output_token,
            OM_uint32 * ret_flags,
            OM_uint32 * time_rec
    )
{
    OM_uint32 ret;
    krb5_data indata;
    NegTokenTarg targ;
    u_char oidbuf[17];
    size_t oidlen;
    gss_buffer_desc sub_token;
    ssize_t mech_len;
    const u_char *p;
    size_t len, taglen;
    krb5_boolean require_mic;

    output_token->length = 0;
    output_token->value  = NULL;

    /*
     * SPNEGO doesn't include gss wrapping on SubsequentContextToken
     * like the Kerberos 5 mech does. But lets check for it anyway.
     */
    
    mech_len = gssapi_krb5_get_mech (input_token->value,
				     input_token->length,
				     &p);

    if (mech_len < 0) {
	indata.data = input_token->value;
	indata.length = input_token->length;
    } else if (mech_len == GSS_KRB5_MECHANISM->length
	&& memcmp(GSS_KRB5_MECHANISM->elements, p, mech_len) == 0)
	return gsskrb5_init_sec_context (minor_status,
					 initiator_cred_handle,
					 context_handle,
					 target_name,
					 GSS_KRB5_MECHANISM,
					 req_flags,
					 time_req,
					 input_chan_bindings,
					 input_token,
					 actual_mech_type,
					 output_token,
					 ret_flags,
					 time_rec);
    else if (mech_len == GSS_SPNEGO_MECHANISM->length
	     && memcmp(GSS_SPNEGO_MECHANISM->elements, p, mech_len) == 0){
	ret = _gssapi_decapsulate (minor_status,
				   input_token,
				   &indata,
				   GSS_SPNEGO_MECHANISM);
	if (ret)
	    return ret;
    } else
	return GSS_S_BAD_MECH;

    ret = der_match_tag_and_length((const char *)indata.data,
				   indata.length,
				   ASN1_C_CONTEXT, CONS, 1, &len, &taglen);
    if (ret) {
	gssapi_krb5_set_status("Failed to decode NegToken choice");
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    if(len > indata.length - taglen) {
	gssapi_krb5_set_status("Buffer overrun in NegToken choice");
	*minor_status = ASN1_OVERRUN;
	return GSS_S_FAILURE;
    }

    ret = decode_NegTokenTarg((const char *)indata.data + taglen, 
			      len, &targ, NULL);
    if (ret) {
	gssapi_krb5_set_status("Failed to decode NegTokenTarg");
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    if (targ.negResult == NULL
	|| *(targ.negResult) == reject
	|| targ.supportedMech == NULL) {
	free_NegTokenTarg(&targ);
	return GSS_S_BAD_MECH;
    }
    
    ret = der_put_oid(oidbuf + sizeof(oidbuf) - 1,
		      sizeof(oidbuf),
		      targ.supportedMech,
		      &oidlen);
    if (ret || oidlen != GSS_KRB5_MECHANISM->length
	|| memcmp(oidbuf + sizeof(oidbuf) - oidlen,
		  GSS_KRB5_MECHANISM->elements,
		  oidlen) != 0) {
	free_NegTokenTarg(&targ);
	return GSS_S_BAD_MECH;
    }

    if (targ.responseToken != NULL) {
	sub_token.length = targ.responseToken->length;
	sub_token.value  = targ.responseToken->data;
    } else {
	sub_token.length = 0;
	sub_token.value  = NULL;
    }

    ret = gsskrb5_init_sec_context(minor_status,
				   initiator_cred_handle,
				   context_handle,
				   target_name,
				   GSS_KRB5_MECHANISM,
				   req_flags,
				   time_req,
				   input_chan_bindings,
				   &sub_token,
				   actual_mech_type,
				   output_token,
				   ret_flags,
				   time_rec);
    if (ret) {
	free_NegTokenTarg(&targ);
	return ret;
    }

    /*
     * Verify the mechListMIC if CFX was used; or if local policy
     * dictated so.
     */
    ret = _gss_spnego_require_mechlist_mic(minor_status, *context_handle,
					   &require_mic);
    if (ret) {
	free_NegTokenTarg(&targ);
	return ret;
    }

    if (require_mic) {
	MechTypeList mechlist;
	MechType m0;
	size_t buf_len;
	gss_buffer_desc mic_buf, mech_buf;

	if (targ.mechListMIC == NULL) {
	    free_NegTokenTarg(&targ);
	    *minor_status = 0;
	    return GSS_S_BAD_MIC;
	}

	mechlist.len = 1;
	mechlist.val = &m0;

	ret = der_get_oid(GSS_KRB5_MECHANISM->elements,
			  GSS_KRB5_MECHANISM->length,
			  &m0,
			  NULL);
	if (ret) {
	    free_NegTokenTarg(&targ);
	    *minor_status = ENOMEM;
	    return GSS_S_FAILURE;
	}

	ASN1_MALLOC_ENCODE(MechTypeList, mech_buf.value, mech_buf.length,
			   &mechlist, &buf_len, ret);
	if (ret) {
	    free_NegTokenTarg(&targ);
	    free_oid(&m0);
	    *minor_status = ENOMEM;
	    return GSS_S_FAILURE;
	}
	if (mech_buf.length != buf_len)
	    abort();

	mic_buf.length = targ.mechListMIC->length;
	mic_buf.value  = targ.mechListMIC->data;

	ret = gss_verify_mic(minor_status, *context_handle,
			     &mech_buf, &mic_buf, NULL);
	free(mech_buf.value);
	free_oid(&m0);
    }
    free_NegTokenTarg(&targ);
    return ret;
}

static OM_uint32
spnego_initial
           (OM_uint32 * minor_status,
            const gss_cred_id_t initiator_cred_handle,
            gss_ctx_id_t * context_handle,
            const gss_name_t target_name,
            const gss_OID mech_type,
            OM_uint32 req_flags,
            OM_uint32 time_req,
            const gss_channel_bindings_t input_chan_bindings,
            const gss_buffer_t input_token,
            gss_OID * actual_mech_type,
            gss_buffer_t output_token,
            OM_uint32 * ret_flags,
            OM_uint32 * time_rec
	   )
{
    NegTokenInit ni;
    int ret;
    OM_uint32 sub, minor;
    gss_buffer_desc mech_token;
    u_char *buf;
    size_t buf_size, buf_len;
    krb5_data data;

    memset (&ni, 0, sizeof(ni));

    ALLOC(ni.mechTypes, 1);
    if (ni.mechTypes == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    ALLOC_SEQ(ni.mechTypes, 1);
    if (ni.mechTypes->val == NULL) {
	free_NegTokenInit(&ni);
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    ret = der_get_oid(GSS_KRB5_MECHANISM->elements,
		      GSS_KRB5_MECHANISM->length,
		      &ni.mechTypes->val[0],
		      NULL);
    if (ret) {
	free_NegTokenInit(&ni);
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }

#if 0
    ALLOC(ni.reqFlags, 1);
    if (ni.reqFlags == NULL) {
	free_NegTokenInit(&ni);
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    ni.reqFlags->delegFlag    = req_flags & GSS_C_DELEG_FLAG;
    ni.reqFlags->mutualFlag   = req_flags & GSS_C_MUTUAL_FLAG;
    ni.reqFlags->replayFlag   = req_flags & GSS_C_REPLAY_FLAG;
    ni.reqFlags->sequenceFlag = req_flags & GSS_C_SEQUENCE_FLAG;
    ni.reqFlags->anonFlag     = req_flags & GSS_C_ANON_FLAG;
    ni.reqFlags->confFlag     = req_flags & GSS_C_CONF_FLAG;
    ni.reqFlags->integFlag    = req_flags & GSS_C_INTEG_FLAG;
#else
    ni.reqFlags = NULL;
#endif

    sub = gsskrb5_init_sec_context(&minor,
				   initiator_cred_handle,
				   context_handle,
				   target_name,
				   GSS_KRB5_MECHANISM,
				   req_flags,
				   time_req,
				   input_chan_bindings,
				   GSS_C_NO_BUFFER,
				   actual_mech_type,
				   &mech_token,
				   ret_flags,
				   time_rec);
    if (GSS_ERROR(sub)) {
	free_NegTokenInit(&ni);
	return sub;
    }
    if (mech_token.length != 0) {
	ALLOC(ni.mechToken, 1);
	if (ni.mechToken == NULL) {
	    free_NegTokenInit(&ni);
	    gss_release_buffer(&minor, &mech_token);
	    *minor_status = ENOMEM;
	    return GSS_S_FAILURE;
	}
	ni.mechToken->length = mech_token.length;
	ni.mechToken->data = malloc(mech_token.length);
	if (ni.mechToken->data == NULL && mech_token.length != 0) {
	    free_NegTokenInit(&ni);
	    gss_release_buffer(&minor, &mech_token);
	    *minor_status = ENOMEM;
	    return GSS_S_FAILURE;
	}
	memcpy(ni.mechToken->data, mech_token.value, mech_token.length);
	gss_release_buffer(&minor, &mech_token);
    } else
	ni.mechToken = NULL;

    /* XXX ignore mech list mic for now */
    ni.mechListMIC = NULL;


    {
	NegotiationToken nt;

	nt.element = choice_NegotiationToken_negTokenInit;
	nt.u.negTokenInit = ni;

	ASN1_MALLOC_ENCODE(NegotiationToken, buf, buf_size,
			   &nt, &buf_len, ret);
	if (ret == 0 && buf_size != buf_len)
	    abort();
    }

    data.data   = buf;
    data.length = buf_size;

    free_NegTokenInit(&ni);
    if (ret)
	return ret;

    sub = _gssapi_encapsulate(minor_status,
			      &data,
			      output_token,
			      GSS_SPNEGO_MECHANISM);
    free (buf);

    if (sub)
	return sub;

    return GSS_S_CONTINUE_NEEDED;
}

static OM_uint32
spnego_init_sec_context
           (OM_uint32 * minor_status,
            const gss_cred_id_t initiator_cred_handle,
            gss_ctx_id_t * context_handle,
            const gss_name_t target_name,
            const gss_OID mech_type,
            OM_uint32 req_flags,
            OM_uint32 time_req,
            const gss_channel_bindings_t input_chan_bindings,
            const gss_buffer_t input_token,
            gss_OID * actual_mech_type,
            gss_buffer_t output_token,
            OM_uint32 * ret_flags,
            OM_uint32 * time_rec
           )
{
    if (input_token == GSS_C_NO_BUFFER || input_token->length == 0)
	return spnego_initial (minor_status,
			       initiator_cred_handle,
			       context_handle,
			       target_name,
			       mech_type,
			       req_flags,
			       time_req,
			       input_chan_bindings,
			       input_token,
			       actual_mech_type,
			       output_token,
			       ret_flags,
			       time_rec);
    else
	return spnego_reply (minor_status,
			     initiator_cred_handle,
			     context_handle,
			     target_name,
			     mech_type,
			     req_flags,
			     time_req,
			     input_chan_bindings,
			     input_token,
			     actual_mech_type,
			     output_token,
			     ret_flags,
			     time_rec);
}

/*
 * gss_init_sec_context
 */

OM_uint32 gss_init_sec_context
           (OM_uint32 * minor_status,
            const gss_cred_id_t initiator_cred_handle,
            gss_ctx_id_t * context_handle,
            const gss_name_t target_name,
            const gss_OID mech_type,
            OM_uint32 req_flags,
            OM_uint32 time_req,
            const gss_channel_bindings_t input_chan_bindings,
            const gss_buffer_t input_token,
            gss_OID * actual_mech_type,
            gss_buffer_t output_token,
            OM_uint32 * ret_flags,
            OM_uint32 * time_rec
           )
{
    GSSAPI_KRB5_INIT ();

    output_token->length = 0;
    output_token->value  = NULL;

    if (ret_flags)
	*ret_flags = 0;
    if (time_rec)
	*time_rec = 0;

    if (target_name == GSS_C_NO_NAME) {
	if (actual_mech_type)
	    *actual_mech_type = GSS_C_NO_OID;
	*minor_status = 0;
	return GSS_S_BAD_NAME;
    }

    if (mech_type == GSS_C_NO_OID || 
	gss_oid_equal(mech_type,  GSS_KRB5_MECHANISM))
	return gsskrb5_init_sec_context(minor_status,
					initiator_cred_handle,
					context_handle,
					target_name,
					mech_type,
					req_flags,
					time_req,
					input_chan_bindings,
					input_token,
					actual_mech_type,
					output_token,
					ret_flags,
					time_rec);
    else if (gss_oid_equal(mech_type, GSS_SPNEGO_MECHANISM))
	return spnego_init_sec_context (minor_status,
					initiator_cred_handle,
					context_handle,
					target_name,
					mech_type,
					req_flags,
					time_req,
					input_chan_bindings,
					input_token,
					actual_mech_type,
					output_token,
					ret_flags,
					time_rec);
    else
	return GSS_S_BAD_MECH;
}
