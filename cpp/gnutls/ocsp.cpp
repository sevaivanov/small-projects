// Vsevolod Ivanov

#include <iostream>
#include <fstream>
#include <streambuf>

#include <gnutls/crypto.h>
#include <gnutls/ocsp.h>

#include <opendht.h>
#include <opendht/log.h>
#include <opendht/http.h>

static gnutls_x509_crt_t load_cert(const char* cert_file, unsigned int crt_max = 1, unsigned flags = 0)
{
    int ret;
    size_t size;
	static gnutls_x509_crt_t *crt;
    gnutls_datum_t data;

    std::ifstream cert_stream(cert_file, std::ios::binary | std::ios::in);
    std::string contents((std::istreambuf_iterator<char>(cert_stream)),
                          std::istreambuf_iterator<char>());
    data.data = (unsigned char*) contents.c_str();
    data.size = contents.size();
    if (!data.data){
        printf("Cannot open file: %s\n", cert_file);
        exit(1);
    }
    ret = gnutls_x509_crt_list_import2(&crt, &crt_max, &data, GNUTLS_X509_FMT_PEM, flags);
    if (ret < 0){
        printf("Cannot import certificate in %s: %s\n", cert_file, gnutls_strerror(ret));
        exit(1);
    }
    printf("Loaded %d certificates.\n", (int) crt_max);
    return *crt;
}

void generate_request(gnutls_x509_crt_t cert, gnutls_x509_crt_t issuer,
		              gnutls_datum_t * rdata, gnutls_datum_t *nonce)
{
	gnutls_ocsp_req_t req;
	int ret;
	ret = gnutls_ocsp_req_init(&req);
	if (ret < 0){
		printf( "ocsp_req_init: %s", gnutls_strerror(ret));
		exit(1);
	}
	ret = gnutls_ocsp_req_add_cert(req, GNUTLS_DIG_SHA1, issuer, cert);
	if (ret < 0){
		printf( "ocsp_req_add_cert: %s",
			gnutls_strerror(ret));
		exit(1);
	}
	if (nonce){
		ret = gnutls_ocsp_req_set_nonce(req, 0, nonce);
		if (ret < 0){
			printf( "ocsp_req_set_nonce: %s",
				gnutls_strerror(ret));
			exit(1);
		}
	}
	ret = gnutls_ocsp_req_export(req, rdata);
	if (ret != 0){
		printf( "ocsp_req_export: %s",
			gnutls_strerror(ret));
		exit(1);
	}
	gnutls_ocsp_req_deinit(req);
	return;
}

std::string
static bytes_to_hex_string(const unsigned char* data, const int size)
{
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < size; i++)
        ss << std::setw(2) << std::setfill('0') << (int)data[i];
    return ss.str();
}

static int ocsp_response(gnutls_datum_t* data, gnutls_x509_crt_t cert,
                         gnutls_x509_crt_t signer, gnutls_datum_t* nonce)
{
    gnutls_ocsp_resp_t resp;
    int ret;
    unsigned verify;
    gnutls_datum_t rnonce;
    if (data->size == 0) {
		printf("Received empty response\n");
		exit(1);
	}
    ret = gnutls_ocsp_resp_init(&resp);
    if (ret < 0){
        printf("ocsp_resp_init: %s\n", gnutls_strerror(ret));
        exit(1);
    }
    ret = gnutls_ocsp_resp_import(resp, data);
    if (ret < 0){
        printf("ocsp_resp_import: %s\n", gnutls_strerror(ret));
        exit(1);
    }
    gnutls_datum_t dat;
    ret = gnutls_ocsp_resp_print(resp, GNUTLS_OCSP_PRINT_FULL, &dat);
    if (ret < 0) {
        printf("ocsp_resp_print: %s\n", gnutls_strerror(ret));
        exit(1);
    }
    printf("%s\n", dat.data);
    gnutls_free(dat.data);
    ret = gnutls_ocsp_resp_check_crt(resp, 0, cert);
    if (ret < 0){
        printf("ocsp_resp_check_crt: %s\n", gnutls_strerror(ret));
        exit(1);
    }
    ret = gnutls_ocsp_resp_get_nonce(resp, NULL, &rnonce);
    if (ret < 0)
        exit(1);
    printf("\n\nNonce sent: %s\n", bytes_to_hex_string(nonce->data, nonce->size).c_str());
    printf("Nonce recv: %s\n\n", bytes_to_hex_string(rnonce.data, rnonce.size).c_str());
    if (rnonce.size != nonce->size || memcmp(nonce->data, rnonce.data,
        nonce->size) != 0){
        exit(1);
    }
    ret = gnutls_ocsp_resp_verify_direct(resp, signer, &verify, 0);
    if (ret < 0)
        exit(1);
    printf("Verifying OCSP Response: ");
    if (verify == 0)
        printf("Verification success!\n");
    else
        printf("Verification error!\n");
    if (verify & GNUTLS_OCSP_VERIFY_SIGNER_NOT_FOUND)
        printf("Signer cert not found\n");
    if (verify & GNUTLS_OCSP_VERIFY_SIGNER_KEYUSAGE_ERROR)
        printf("Signer cert keyusage error\n");
    if (verify & GNUTLS_OCSP_VERIFY_UNTRUSTED_SIGNER)
        printf("Signer cert is not trusted\n");
    if (verify & GNUTLS_OCSP_VERIFY_INSECURE_ALGORITHM)
        printf("Insecure algorithm\n");
    if (verify & GNUTLS_OCSP_VERIFY_SIGNATURE_FAILURE)
        printf("Signature failure\n");
    if (verify & GNUTLS_OCSP_VERIFY_CERT_NOT_ACTIVATED)
        printf("Signer cert not yet activated\n");
    if (verify & GNUTLS_OCSP_VERIFY_CERT_EXPIRED)
        printf("Signer cert expired\n");
    gnutls_free(rnonce.data);
    gnutls_ocsp_resp_deinit(resp);
    return verify;
}

int main(int argc, char* argv[])
{
    /* -----------------------------------
     * Online Certificate Service Protocol
     * -----------------------------------
     */
    if (argc < 4){
        printf("./binary <cert_file> <issuer_file> <signer_file>\n");
        return 1;
    }
    int ret;
    std::string cert_file = argv[1];
    std::string issuer_file = argv[2];
    std::string signer_file = argv[3];
    printf("cert: %s issuer: %s signer: %s\n",
            cert_file.c_str(), issuer_file.c_str(), signer_file.c_str());

    // Initialize GnuTLS
    ret = gnutls_global_init();
    if (ret < 0)
        printf( "gnutls_global_init: %s\n", gnutls_strerror(ret));

    gnutls_x509_crt_t cert, issuer, signer;
    // load_cert(file, max, flags |= GNUTLS_X509_CRT_LIST_SORT
    cert = load_cert(cert_file.c_str());
    issuer = load_cert(issuer_file.c_str());
    signer = load_cert(signer_file.c_str());

    auto dcert = dht::crypto::Certificate(cert);
    auto dissuer = dht::crypto::Certificate(issuer);
    auto dsigner = dht::crypto::Certificate(signer);
    printf("Certificate: %s\nIssuer: %s\nSigner: %s\n",
           dcert.getUID().c_str(), dissuer.getUID().c_str(), dsigner.getUID().c_str());

    std::string aia_uri;
    for (int seq = 0;; seq++)
    {
        std::cout << "seq: " << seq << std::endl;
        // Extracts the Authority Information Access (AIA) extension, see RFC 5280 section 4.2.2.1
        gnutls_datum_t tmp;
        ret = gnutls_x509_crt_get_authority_info_access(cert, seq, GNUTLS_IA_OCSP_URI, &tmp, NULL);
        if (ret == GNUTLS_E_UNKNOWN_ALGORITHM)
            continue;
        if (ret == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE){
            printf("No URI was found in the certificate.\n");
            exit(1);
        }
        if (ret < 0){
            printf( "error: %s\n", gnutls_strerror(ret));
            exit(1);
        }
        std::stringstream aia_stream;
        aia_stream.write((const char*)tmp.data, tmp.size);
        aia_uri = aia_stream.str();
        gnutls_free(tmp.data);
        break;
    }
    printf("CA issuers URI: %s\n", aia_uri.c_str());

    // Generate OCSP request
    unsigned char noncebuf[23];
    gnutls_datum_t nonce = { noncebuf, sizeof(noncebuf) };
    ret = gnutls_rnd(GNUTLS_RND_NONCE, nonce.data, nonce.size);
    if (ret < 0){
        printf("gnutls_rnd: %s\n", gnutls_strerror(ret));
        return -1;
    }
    gnutls_datum_t ocsp_req;
	generate_request(cert, issuer, &ocsp_req, &nonce);

    // Send OCSP request
    using namespace dht;
    asio::io_context io_context;
    std::shared_ptr<dht::Logger> logger = dht::log::getStdLogger();
    auto request = std::make_shared<http::Request>(io_context, aia_uri, logger);

    request->set_method(restinio::http_method_post());
    request->set_header_field(restinio::http_field_t::user_agent, "OCSP client");
    request->set_header_field(restinio::http_field_t::accept, "*/*");
    request->set_header_field(restinio::http_field_t::content_type, "application/ocsp-request");
    {
        std::string body;
        std::stringstream body_stream;
        body_stream.write((const char*)ocsp_req.data, ocsp_req.size);
        body = body_stream.str();
        std::cout << body << std::endl;
        request->set_body(body);
        std::ofstream ocsp_req_f ("ocsp-request.bin", std::ios::out | std::ios::binary);
        ocsp_req_f.write (body.c_str(), body.size());
        ocsp_req_f.close();
        gnutls_free(ocsp_req.data);
    }
    request->set_connection_type(restinio::http_connection_header_t::close);

    http::Response http_resp;
    std::weak_ptr<http::Request> wreq = request;
    request->add_on_state_change_callback([logger, &io_context, wreq, &http_resp]
                                          (const http::Request::State state, const http::Response response){
        logger->w("HTTP Request state=%i status_code=%i", state, response.status_code);
        auto request = wreq.lock();
        logger->w("HTTP Request:\n%s", request->to_string().c_str());
        if (state == http::Request::State::SENDING){
            request->get_connection()->timeout(std::chrono::seconds(1),
                [request](const asio::error_code& ec){
                    request->cancel();
                });
        }
        if (state != http::Request::State::DONE)
            return;
        for (auto& it: response.headers)
            std::cout << it.first << ": " << it.second << std::endl;
        std::cout << response.body << std::endl;
        if (response.status_code != 200)
            logger->e("HTTP Request Failed with status_code=%i", response.status_code);
        else
            logger->w("HTTP Request done!");
        http_resp = response;
        io_context.stop();
    });
    request->send();
    auto work = asio::make_work_guard(io_context);
    io_context.run();

    // Read OCSP response
    auto body = http_resp.body;
    unsigned int content_length = stoul(http_resp.headers["Content-Length"]);
    std::cout << "Response body.size=" << body.size() << " Content-Length=" << content_length
              << std::endl << body << std::endl;

    // works with: cat ocsp-response | ocsptool --response-info
    std::ofstream ocsp_resp_f ("ocsp-response.bin", std::ios::out | std::ios::binary);
    ocsp_resp_f.write (body.c_str(), body.size());
    ocsp_resp_f.close();

    gnutls_datum_t ocsp_resp;
    ocsp_resp.data = (unsigned char*)body.c_str();
    ocsp_resp.size = body.size();

    int v = ocsp_response(&ocsp_resp, cert, signer, &nonce);
    printf("= verified: %i\n", v);
    gnutls_global_deinit();
    printf("= success\n");
    return 0;
}
