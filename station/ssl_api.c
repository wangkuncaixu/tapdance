#include "ssl_api.h"
//#include "../util/telex_util.h"
#include <assert.h>

static int fetch_data_from_bio(SSL *s, char **out)
{
    int i;
    BIO *bio = SSL_get_wbio(s);
    if (!bio) {
      fprintf(stderr, "Couldn't get write BIO for SSL object!\n");
      fflush(stderr);
      return -1;
    }
    char *crypted_data;
    long crypted_data_len = BIO_get_mem_data(bio, &crypted_data);
    *out = malloc(crypted_data_len);
    if (!*out) {
        return -1;
    }

    memcpy(*out, crypted_data, crypted_data_len);

    if (BIO_reset(bio) <= 0) {
      fprintf(stderr, "fetch_data_from_bio: BIO_reset returned <= 0\n");
      fflush(stderr);
      return -1;
    }
    i = crypted_data_len;

    return i; 
}

int ssl_encrypt(SSL *s, const char *in, int len, char **out)
{

    int i;

    if (!BIO_eof(SSL_get_wbio(s))) {
      fprintf(stderr, "ssl_encrypt: Someone left data in the wbio!\n");
      fprintf(stderr, "In particular, this data:\n");
      fflush(stderr);
      char *data;
      long data_len = BIO_get_mem_data(SSL_get_wbio(s), &data);
      //hexdump(data, data_len);
      return -1;
    }

    i = SSL_write(s, in, len);
    if (i < 0) {
      fprintf(stderr, "ssl_encrypt: SSL_write returned < 0\n");
      fflush(stderr);
      return -1;
    }

    return fetch_data_from_bio(s, out);
}

int ssl_shutdown(SSL *s, char **out) {
    int i = SSL_shutdown(s);
    if (i < 0) {
      return -1;
    }
    return fetch_data_from_bio(s, out);
}

int ssl_decrypt(SSL *s, const char *in, int len, char **out)
{
    int i;
    
    // WARNING: we expect this to be a memory bio...
    BIO *rbio = SSL_get_rbio(s);
    if (!BIO_eof(rbio)) {
      fprintf(stderr, "ssl_decrypt: Someone left data in the rbio!");
      fflush(stderr);
      return -1;
    }

    if (BIO_write(rbio, in, len) != len) {
      fprintf(stderr, "ssl_decrypt: couldn't write to BIO!");
      fflush(stderr);
      return -1;
    }

    *out = malloc(len);
    if (!*out) {
        return -1;
    }

    i = 0;
    int ret = SSL_read(s, *out + i, len - i);
    i += ret;  // If <= 0, we return it; o.w. we should accumulate.
    fprintf(stderr, "first SSL_read returned: %d\n", ret);
    fflush(stderr);
    while (ret > 0 && !BIO_eof(rbio) && i < len) {
      ret = SSL_read(s, *out + i, len - i);
      fprintf(stderr, "SSL_read returned: %d\n", ret);
      fflush(stderr);
      if (ret > 0)
          i += ret;
    }

    if (!BIO_eof(rbio)) {
      fprintf(stderr, "We are leaving data in the rbio! ret: %d\n", ret);
      fprintf(stderr, "In particular, this data:\n");
      char *data;
      long data_len = BIO_get_mem_data(rbio, &data);
      //hexdump(data, data_len);
    }

    return i;
}

// Inputs a 16-byte telex_secret (generated by gen_tag's key output)
// and produces a 1023-bit bignum to be used as the client's dh_priv_key
// Uses Krawczyk's crypto-correct PRG: http://eprint.iacr.org/2010/264
// page 11, PRK = state_secret, CTXinfo = uniq
BIGNUM *telex_ssl_get_dh_key(void *telex_secret, BIGNUM *res)
{
    int i;
    char *uniq = "Telex PRG";
    unsigned char buf[128];
    unsigned char out[SHA256_DIGEST_LENGTH];
    unsigned char in[128]; // > SHA256_DIGEST_LENTH + strlen(uniq) + sizeof(int)
    unsigned int out_len, in_len;

    // buf will end up with
    // x_{1...4}
    // x_(i+1) = HMAC{state_secret}(x_i | uniq | i)
    // uniq = "Telex PRG"
    // x_0 = empty string 

    memset(out, 0, sizeof(out));
    out_len = 0;    // x_0 = ""
    for (i=0; i<sizeof(buf)/SHA256_DIGEST_LENGTH; i++) {
        // Load the input for the hmac: x_i | uniq | i
        in_len = 0;
        memcpy(&in[in_len], out, out_len);
        in_len += out_len;

        memcpy(&in[in_len], uniq, strlen(uniq));
        in_len += strlen(uniq);

        memcpy(&in[in_len], &i, sizeof(i));
        in_len += sizeof(i);

        HMAC(EVP_sha256(),
            (unsigned char*)telex_secret, 16,
            in, in_len,
            out, &out_len);
        assert(out_len == SHA256_DIGEST_LENGTH);
        memcpy(&buf[i*SHA256_DIGEST_LENGTH], out, out_len);
    }

    //from bnrand, they do this for bits=1023, top=0, bottom=0.
    buf[0] |= (1<<6);
    buf[0] &= 0x7f;

    return BN_bin2bn(buf, sizeof(buf), res);
}

// shared_secret should be allocated to hold at least 128-bytes (256-bytes for 2048 bit dh).
// given the priv_key from the client, the agreed upon modulus p, and the server's dh_pub_key,
// this will generate the shared secret
int telex_get_shared_secret(BIGNUM *client_dh_priv_key, BIGNUM *p, BIGNUM *server_dh_pub_key, char *shared_secret)
{
    //server_dh_pub_key ^ client_dh_priv_key % p
    int n;
    DH *dh_clnt;

    dh_clnt = DH_new(); // Sets dh_clnt->meth.
    if (!dh_clnt) {
        printf("Error: can't DH_new() in telex_get_shared_secret\n");
        return -1;
    }

    dh_clnt->p = p;
    dh_clnt->priv_key = client_dh_priv_key;

    n = DH_compute_key((unsigned char*)shared_secret, server_dh_pub_key, dh_clnt); 

    DH_free(dh_clnt);

    return n;
}

//key: 32 bytes
//iv: 16 bytes
//mac_secret: 20 bytes
SSL* get_live_ssl_obj(char *master_key, size_t master_key_len, uint16_t cipher_suite, unsigned char *server_random, unsigned char *client_random)
{
    static SSL_CTX *ctx = 0;
    // TODO(ewust/SPTelex): probably having a single context is ok?
    if (!ctx) {
      /* Build our SSL context*/
      //ctx=initialize_ctx(KEYFILE,PASSWORD);
      SSL_library_init();
      SSL_load_error_strings();
      //extern BIO *bio_err;
    //bio_err = BIO_new_fp(stderr, BIO_NOCLOSE);
      ctx = SSL_CTX_new(TLSv1_2_server_method());
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
      fprintf(stderr, "could not init ssl...\n");
      exit(-1);
    }

    //ssl->telex_client_random = "hello, worldxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n";

    //if (is_server) {
      ssl->type = SSL_ST_ACCEPT;
      ssl->method = TLSv1_2_server_method();
    /*} else {
      ssl->type = SSL_ST_CONNECT;
      ssl->method = TLSv1_client_method();
    } */
    ssl->rwstate = SSL_NOTHING;
    ssl->in_handshake = 0; /* We're simulating having finished SSL_connect. */
    ssl->handshake_func = ssl3_connect;
    ssl->server = 0;

    ssl->new_session = 0;
    ssl->quiet_shutdown = 0;
    ssl->shutdown = 0;

    ssl->state = SSL_ST_OK;
    ssl->rstate = SSL_ST_READ_HEADER;

    /* Handshake stuff, we're not doing a handshake. */
    ssl->init_buf = ssl->init_msg = NULL;
    ssl->init_num = ssl->init_off = 0;

    assert(ssl->packet == NULL);
    assert(ssl->packet_length == 0);

    // Set by SSL_new.
    assert(ssl->read_ahead == 0);
    assert(ssl->msg_callback == NULL);

    // Set by SSL_clear.
    assert(ssl->hit == 0);

    // Let's not touch ssl->param; it's set by SSL_new.

    // Set by SSL_new.
    assert(ssl->cipher_list == NULL);
    assert(ssl->cipher_list_by_id == NULL);

    //assert(ssl->mac_flags == 0);

    // Set by SSL clear.
    assert(ssl->enc_read_ctx == NULL);
    assert(ssl->read_hash == NULL);
    assert(ssl->expand == NULL);
    assert(ssl->enc_write_ctx == NULL);
    assert(ssl->write_hash == NULL);
    assert(ssl->compress == NULL);

    // Let's leave ssl->cert alone; should be set by SSL_new.
    // Let's leave ssl->sid_ctx_length alone; should be set by SSL_new.

    if (!ssl_get_new_session(ssl, 0)) {
      fprintf(stderr, "Couldn't get session\n");
      exit(-1);
    }

    // verify_mode and verify_callback are set by SSL_new.

    assert(ssl->info_callback == NULL); // ?
    assert(ssl->error == 0);
    assert(ssl->error_code == 0);
    //assert(ssl->psk_client_callback == NULL);
    //assert(ssl->psk_server_callback == NULL);

    assert(ssl->ctx);

    ssl->verify_result = X509_V_OK;  // Just say the cert's valid.

    // ssl->ex_data is a TODO.
    // Set by SSL_new (big memset to 0).
    assert(ssl->client_CA == NULL);

    assert(ssl->references == 1);
    //ssl->options is set in SSL_new.
    //ssl->mode is set in SSL_new.
    //ssl->max_cert_list is set in SSL_new.

    // TODO(ewust/SPTelex): would this be TLS 1.0 or TLS 1.2?
    ssl->client_version = ssl->version;
    //  ssl->max_send_fragment is set in SSL_new.

    // In SSL_new:
    assert(ssl->tlsext_status_type == -1);
    assert(ssl->tlsext_status_expected == 0);
    assert(ssl->servername_done == 0);
    assert(ssl->tlsext_ocsp_resplen == -1);
    assert(ssl->tlsext_ticket_expected == 0);

    // TODO(ewust/SPTelex): might be needed; but we already have master key?
#if 0
    // The elliptic curve stuff below probably isn't necessary if
    // we're not using an elliptic curve cipher.
    ssl->tlsext_ecpointformatlist_length = 3;

    ssl->tlsext_ecpointformatlist = OPENSSL_malloc(3);
    memcpy(ssl->tlsext_ecpointformatlist, "\x00\x01\x02", 3);
    ssl->tlsext_ellipticcurvelist_length = 50;
    
    ssl->tlsext_ellipticcurvelist = OPENSSL_malloc(50);
    memcpy(ssl->tlsext_ellipticcurvelist, "\x00\x01\x00\x02\x00\x03\x00" 
           "\x04\x00\x05\x00\x06\x00\x07\x00\x08\x00\x09\x00\x0a\x00\x0b\x00"
           "\x0c\x00\x0d\x00\x0e\x00\x0f\x00\x10\x00\x11\x00\x12\x00\x13\x00"
           "\x14\x00\x15\x00\x16\x00\x17\x00\x18\x00\x19", 50);
#endif

    //ssl->tls_opaque_prf_input_len = 0;
    
    ssl->initial_ctx = ssl->ctx;
    

    // This is done by ssl3_new.
    //memset(ssl->s3, 0, sizeof(*ssl->s3));


    // TODO(SPTelex): don't think 1.2 in GCM needs this
    ssl->s3->need_empty_fragments = 1;

    //SSL3_BUFFER is {buf, len, offset, left};
    //ssl->s3->rbuf.buf = NULL; //TODO these buffers are set up by first call to read
    //ssl->s3->rrec = 0x16; //TODO?
    //ssl->s3->wbuf.buf = NULL; //TODO same with these for first call to write
    //ssl->s3->wrec = 0x16; //TODO?

    // swolchok: We don't want to set this stuff, it's for pending writes.
#if 0
    ssl->s3->wpend_tot = 16;
    ssl->s3->wpend_type = 22;
    ssl->s3->wpend_ret = 16;
#endif
    
    ssl->s3->wpend_buf = NULL; //TODO

    ssl3_setup_buffers(ssl);

    //assert(ssl->s3->handshake_dgst == NULL); //TODO; swolchok: should be free.


    ssl->s3->tmp.message_size = 12;
    ssl->s3->tmp.message_type = 20;
    ssl->s3->tmp.new_cipher = NULL; //TODO

    ssl->s3->tmp.next_state = 4576; //hehe
    ssl->s3->tmp.new_sym_enc = NULL; //TODO
    ssl->s3->tmp.new_hash = NULL;   //TODO

//ssl->s3->tmp.new_mac_pkey_type = 855;
//ssl->s3->tmp.new_mac_secret_size = 20;

    if (!ssl_get_new_session(ssl, 0)) {
      fprintf(stderr, "Couldn't initialize session\n");
      exit(-1);
    }

#if 0
        c = malloc(sizeof(EVP_CIPHER));
        //ssl->s3->tmp.new_sym_enc = c;
        
        c->nid = 427;
        c->block_size = 16;
        c->key_len = 32;
        c->iv_len = 16;
        c->flags = 2;
        //c->init = aes_init_key;
        //c->do_cipher = aes_256_cbc_cipher;
        c->cleanup = 0;
        c->ctx_size = 244;
        //c->set_asn1_parameters = EVP_CIPHER_set_asn1_iv;
        //c->get_asn1_parameters = EVP_CIPHER_get_asn1_iv;
        c->ctrl = 0;
        c->app_data = NULL;
#endif

#if 0
        memset(ssl->enc_write_ctx, 0, sizeof(EVP_CIPHER_CTX));
        ssl->enc_write_ctx->cipher = NULL;
        ssl->enc_write_ctx->engine = NULL;
        ssl->enc_write_ctx->encrypt = 1;
        ssl->enc_write_ctx->buf_len = 0;
        ssl->enc_write_ctx->oiv; //TODO
        ssl->enc_write_ctx->iv; //TODO
        ssl->enc_write_ctx->key_len = 32;
        ssl->enc_write_ctx->cipher_data; //TODO
        ssl->enc_write_ctx->block_mask = 0x0f; 
 

        memset(ssl->enc_read_ctx, 0, sizeof(EVP_CIPHER_CTX));
        ssl->enc_read_ctx->cipher = NULL;  //they can share, for all i care
        ssl->enc_read_ctx->engine = NULL;
        ssl->enc_read_ctx->encrypt = 0;
        ssl->enc_read_ctx->buf_len = 0;
        ssl->enc_read_ctx->oiv; //TODO
        ssl->enc_read_ctx->iv; //TODO
        ssl->enc_read_ctx->key_len = 32;
        ssl->enc_read_ctx->cipher_data; //TODO
        ssl->enc_read_ctx->block_mask = 0x0f;
       

        EVP_CipherInit_ex(ssl->enc_write_ctx, c, NULL, &ssl->s3->tmp.key_block[40],
                          &ssl->s3->tmp.key_block[104], 1);

        EVP_CipherInit_ex(ssl->enc_read_ctx, c, NULL, &ssl->s3->tmp.key_block[72],
                          &ssl->s3->tmp.key_block[120], 0);
#endif

        //BIO setup
        SSL_set_bio(ssl, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));

#if 0  // Don't need to copy in secrets now, but the layout is handy to know.
        memcpy(&ssl->s3->tmp.key_block[0], write->mac_secret, 20);
        memcpy(&ssl->s3->tmp.key_block[40], write->key, 32);
        memcpy(&ssl->s3->tmp.key_block[104], write->iv, 16);

        memcpy(&ssl->s3->tmp.key_block[20], read->mac_secret, 20);
        memcpy(&ssl->s3->tmp.key_block[72], read->key, 32);
        memcpy(&ssl->s3->tmp.key_block[120], read->iv, 16);
#endif
        // TODO(ewust/SPTelex)
        memcpy(ssl->s3->client_random, client_random, 32);
        memcpy(ssl->s3->server_random, server_random, 32);

        if (!switch_to_telex_crypto(ssl, master_key,
                                    master_key_len, cipher_suite)) {
          fprintf(stderr, "Couldn't change to telex crypto!\n");
          exit(-1);
        }
    return ssl;
}


int switch_to_telex_crypto(SSL *ssl, char *master_key, size_t master_key_len,
                           uint16_t cipher_suite) {
    // SSL record sequence numbers should be 1; we just got done with
    // a round of hellos (unless we are using TELEX_LEAK_KEY).
    //if (is_server) {
      ssl->type = SSL_ST_ACCEPT;
      ssl->method = TLSv1_2_server_method();
    /*
    } else {
      ssl->type = SSL_ST_CONNECT;
      ssl->method = TLSv1_client_method();
    }
    */

    memset(ssl->s3->read_sequence, 0, sizeof(ssl->s3->read_sequence));
    memset(ssl->s3->write_sequence, 0, sizeof(ssl->s3->write_sequence));


    //memcpy(ssl->s3->server_random, "La la la some moresecrets forus tokeepi guess this is just random", 32);
    //memcpy(ssl->s3->client_random, "aasdfkjaskljfwamefkamwemcaksd;lcajwlewlekecawmecmaseda;w23i23rjasf", 32);

    // ewust: I don't think this is a todo, as previous_{client,server}_finished
    //                  applies to session renegotiation (t1_reneg.c is the only use)
    // TODO(swolchok): previous_client_finished, previous_server_finished,
    //                 and tmp.finish_md are supposed to be MACs. Probably fine
    //                 as long as we swap them on the client and the server...
    ssl->s3->previous_client_finished_len = 12;
    memcpy(ssl->s3->previous_client_finished, "somefinishedbusiness,ya", 12);
    
    ssl->s3->previous_server_finished_len = 12;
    memcpy(ssl->s3->previous_server_finished, "jsadfkjwefjaewmfamsawe", 12); 

    memcpy(ssl->s3->tmp.finish_md, "akjwemawmefmawe", 12);

    ssl->s3->tmp.finish_md_len = 12;

    // (was DHE-RSA-AES256-SHA) \x00\x39 ...
    // now we select our own     
    ssl->s3->tmp.new_cipher = (SSL_CIPHER*)ssl3_get_cipher_by_char((const unsigned char *)&cipher_suite);
    ssl->session->cipher = ssl->s3->tmp.new_cipher;

    /*
    ssl->session->master_key_length = \
      tls1_generate_master_secret(ssl,
                                  ssl->session->master_key,
                                  telex_secret, telex_secret_length);
    */
    ssl->session->master_key_length = master_key_len;
    memcpy(ssl->session->master_key, master_key, master_key_len);
    // Woo! That felt good.
    //hexdump(ssl->session->master_key, ssl->session->master_key_length);

    //memset(telex_secret, 0, telex_secret_length); // Sore wa himitsu desu!

    if (!tls1_setup_key_block(ssl)) {
      fprintf(stderr, "Couldn't set up key block\n");
      exit(-1);
    }


    // These guys reset ssl->s3->write_sequence and read_sequence respectively....(what else)
    if (!tls1_change_cipher_state(ssl, SSL3_CHANGE_CIPHER_SERVER_WRITE)) {
      fprintf(stderr, "Couldn't change write cipher state\n");
      return 0;
    }
    if (!tls1_change_cipher_state(ssl, SSL3_CHANGE_CIPHER_SERVER_READ)) {
      fprintf(stderr, "Couldn't change read cipher state\n");
      return 0;
    }


    //tls1_final_finish_mac ?

/* For TELEX_LEAK_KEY, we have to "consume" the client_finished message,
    (and "send" the server finished message). This will increase read/write_sequence,
    as well as change the working iv's for ssl->enc_{write,read}_ctx->iv */
   //TODO(ewust): set working iv's here (and possibly remove the following)


    ssl->s3->read_sequence[7] = '\x01';
    ssl->s3->write_sequence[7] = '\x01';

    // IVs (in cbc mode) are simply the last 16-bytes of ciphertext over the wire.

    return 1;
}

