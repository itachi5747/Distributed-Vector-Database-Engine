#include "src/server/http_api.hpp"
#include "src/shard/shard_manager.hpp"
#include "src/hnsw/index.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace vecdb {

// ── MetricsStore ──────────────────────────────────────────────────────────────

void MetricsStore::record_search(uint64_t us) {
    ++total_searches; ++searches_window;
    search_latency_us_sum += us;
    static const uint64_t bounds[N_BUCKETS] = {100,200,300,500,750,1000,2000,UINT64_MAX};
    for (int i = 0; i < N_BUCKETS; ++i)
        if (us < bounds[i]) { ++latency_buckets[i]; break; }
}

void MetricsStore::record_insert(uint64_t us) {
    ++total_inserts; ++inserts_window;
    insert_latency_us_sum += us;
}

void MetricsStore::compute_percentiles() {
    uint64_t counts[N_BUCKETS], total = 0;
    for (int i = 0; i < N_BUCKETS; ++i) { counts[i] = latency_buckets[i].load(); total += counts[i]; }
    if (total > 0) {
        static const uint64_t mid[N_BUCKETS] = {50,150,250,400,625,875,1500,3000};
        uint64_t cum = 0; bool p50=false,p95=false,p99=false;
        for (int i = 0; i < N_BUCKETS; ++i) {
            cum += counts[i]; double f=(double)cum/total;
            if (!p50&&f>=0.50){search_p50_us=mid[i];p50=true;}
            if (!p95&&f>=0.95){search_p95_us=mid[i];p95=true;}
            if (!p99&&f>=0.99){search_p99_us=mid[i];p99=true;}
        }
    }
    inserts_last_sec  = inserts_window.exchange(0);
    searches_last_sec = searches_window.exchange(0);
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

HttpApiServer::HttpApiServer(ShardManager& mgr, MetricsStore& metrics, uint16_t port)
    : mgr_(mgr), metrics_(metrics), port_(port) {}

HttpApiServer::~HttpApiServer() { stop(); }

void HttpApiServer::start() { running_=true; thread_=std::thread(&HttpApiServer::run,this); }

void HttpApiServer::stop() {
    running_=false;
    if (server_fd_>=0){::close(server_fd_);server_fd_=-1;}
    if (thread_.joinable()) thread_.join();
}

// ── Server loop ───────────────────────────────────────────────────────────────

void HttpApiServer::run() {
    server_fd_ = ::socket(AF_INET,SOCK_STREAM,0);
    if (server_fd_<0){fprintf(stderr,"[http] socket() failed: %s\n",strerror(errno));return;}
    int opt=1;
    ::setsockopt(server_fd_,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(port_);
    if (::bind(server_fd_,(sockaddr*)&addr,sizeof(addr))<0){
        fprintf(stderr,"[http] bind() failed on port %d: %s\n",port_,strerror(errno));return;}
    if (::listen(server_fd_,16)<0){
        fprintf(stderr,"[http] listen() failed: %s\n",strerror(errno));return;}
    fprintf(stderr,"[http] Listening on port %d\n",port_);
    ::fcntl(server_fd_,F_SETFL,O_NONBLOCK);

    std::thread ticker([this](){
        while(running_){
            std::this_thread::sleep_for(std::chrono::seconds(1));
            metrics_.compute_percentiles();
        }
    });

    while(running_){
        sockaddr_in cli{}; socklen_t cli_len=sizeof(cli);
        int cfd=::accept(server_fd_,(sockaddr*)&cli,&cli_len);
        if(cfd<0){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
        handle_client(cfd);
        ::close(cfd);
    }
    ticker.join();
}

// ── Request dispatch ──────────────────────────────────────────────────────────

void HttpApiServer::handle_client(int fd) {
    char buf[16384]={};
    int n=::recv(fd,buf,sizeof(buf)-1,0);
    if(n<=0)return;
    std::string req(buf,n);
    std::string method=parse_method(req);
    std::string path  =parse_path(req);
    std::string qstr  =parse_query(req);

    // CORS preflight
    if(method=="OPTIONS"){
        std::string pre="HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(fd,pre.c_str(),pre.size(),0); return;
    }

    std::string response;
    if      (path=="/api/health")                      response=ok_response(handle_health());
    else if (path=="/api/info")                        response=ok_response(handle_info());
    else if (path=="/api/metrics")                     response=ok_response(handle_metrics());
    else if (path=="/api/search"  &&method=="POST")    response=ok_response(handle_search(parse_body(req)));
    else if (path=="/api/insert"  &&method=="POST")    response=ok_response(handle_insert(parse_body(req)));
    else if (path=="/api/node")                        response=ok_response(handle_node(qstr));
    else                                               response=err_response(404,"Not found");

    ::send(fd,response.c_str(),response.size(),0);
}

// ── Parsers ───────────────────────────────────────────────────────────────────

std::string HttpApiServer::parse_method(const std::string& req) {
    return req.substr(0,req.find(' '));
}

std::string HttpApiServer::parse_path(const std::string& req) {
    size_t s=req.find(' ')+1, e=req.find(' ',s);
    std::string p=req.substr(s,e-s);
    size_t q=p.find('?'); if(q!=std::string::npos) p=p.substr(0,q);
    return p;
}

std::string HttpApiServer::parse_query(const std::string& req) {
    size_t s=req.find(' ')+1, e=req.find(' ',s);
    std::string p=req.substr(s,e-s);
    size_t q=p.find('?'); if(q==std::string::npos) return "";
    return p.substr(q+1);
}

std::string HttpApiServer::parse_body(const std::string& req) {
    size_t pos=req.find("\r\n\r\n");
    if(pos==std::string::npos)return "";
    return req.substr(pos+4);
}

// ── HTTP response builders ────────────────────────────────────────────────────

std::string HttpApiServer::ok_response(const std::string& json) {
    std::ostringstream r;
    r<<"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
     <<"Content-Length: "<<json.size()<<"\r\n"
     <<"Access-Control-Allow-Origin: *\r\n"
     <<"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
     <<"Access-Control-Allow-Headers: Content-Type\r\n"
     <<"Connection: close\r\n\r\n"<<json;
    return r.str();
}

std::string HttpApiServer::err_response(int code,const std::string& msg) {
    std::string json="{\"error\":\""+msg+"\"}";
    std::ostringstream r;
    r<<"HTTP/1.1 "<<code<<" Error\r\nContent-Type: application/json\r\n"
     <<"Content-Length: "<<json.size()<<"\r\n"
     <<"Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"<<json;
    return r.str();
}

// ── Route handlers ────────────────────────────────────────────────────────────

std::string HttpApiServer::handle_health() { return R"({"status":"ok"})"; }

std::string HttpApiServer::handle_info() {
    uint64_t total=mgr_.total_vectors();
    uint32_t sc=mgr_.shard_count();
    int dim=mgr_.cfg_dim();
    auto dist=mgr_.load_distribution();
    std::ostringstream j;
    j<<"{\"total_vectors\":"<<total<<",\"shard_count\":"<<sc<<",\"dim\":"<<dim<<",\"shards\":[";
    for(uint32_t s=0;s<sc;++s){
        if(s>0)j<<",";
        j<<"{\"shard_id\":"<<s<<",\"vector_count\":"<<dist[s]
         <<",\"status\":\""<<(mgr_.shard_healthy(s)?"healthy":"degraded")<<"\"}";
    }
    j<<"]}"; return j.str();
}

std::string HttpApiServer::handle_metrics() {
    auto dist=mgr_.load_distribution();
    uint32_t sc=mgr_.shard_count();
    auto now_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream j;
    j<<"{\"total_vectors\":"  <<mgr_.total_vectors()
     <<",\"insert_rate\":"    <<metrics_.inserts_last_sec.load()
     <<",\"search_rate\":"    <<metrics_.searches_last_sec.load()
     <<",\"p50_us\":"         <<metrics_.search_p50_us.load()
     <<",\"p95_us\":"         <<metrics_.search_p95_us.load()
     <<",\"p99_us\":"         <<metrics_.search_p99_us.load()
     <<",\"total_inserts\":"  <<metrics_.total_inserts.load()
     <<",\"total_searches\":" <<metrics_.total_searches.load()
     <<",\"ts\":"             <<now_ms
     <<",\"shards\":[";
    for(uint32_t s=0;s<sc;++s){
        if(s>0)j<<",";
        j<<"{\"id\":"<<s<<",\"vectors\":"<<dist[s]
         <<",\"status\":\""<<(mgr_.shard_healthy(s)?"healthy":"degraded")<<"\"}";
    }
    j<<"]}"; return j.str();
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

static std::vector<float> parse_float_array(const std::string& json,const std::string& key){
    std::vector<float> res;
    size_t kp=json.find("\""+key+"\""); if(kp==std::string::npos)return res;
    size_t as=json.find('[',kp);        if(as==std::string::npos)return res;
    size_t ae=json.find(']',as);        if(ae==std::string::npos)return res;
    std::string arr=json.substr(as+1,ae-as-1);
    std::istringstream ss(arr); std::string tok;
    while(std::getline(ss,tok,',')) try{res.push_back(std::stof(tok));}catch(...){}
    return res;
}

static int64_t parse_int64_field(const std::string& json,const std::string& key,int64_t def){
    size_t kp=json.find("\""+key+"\""); if(kp==std::string::npos)return def;
    size_t cp=json.find(':',kp);        if(cp==std::string::npos)return def;
    try{return std::stoll(json.substr(cp+1));}catch(...){return def;}
}

static std::string parse_query_param(const std::string& qstr,const std::string& key){
    // qstr looks like "id=42&foo=bar"
    size_t pos=qstr.find(key+"=");
    if(pos==std::string::npos)return "";
    size_t start=pos+key.size()+1;
    size_t end=qstr.find('&',start);
    if(end==std::string::npos)return qstr.substr(start);
    return qstr.substr(start,end-start);
}

// ── /api/search ───────────────────────────────────────────────────────────────

std::string HttpApiServer::handle_search(const std::string& body){
    auto query=parse_float_array(body,"query");
    int top_k=(int)parse_int64_field(body,"top_k",5);
    if(query.empty())return R"({"error":"missing query array"})";
    if((int)query.size()!=mgr_.cfg_dim()){
        std::ostringstream e;
        e<<"{\"error\":\"query dim="<<query.size()<<" but index dim="<<mgr_.cfg_dim()<<"\"}";
        return e.str();
    }
    if(top_k<=0||top_k>1000)top_k=5;
    auto t0=std::chrono::high_resolution_clock::now();
    auto sr=mgr_.search(query.data(),top_k);
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now()-t0).count();
    metrics_.record_search((uint64_t)us);
    std::ostringstream j;
    j<<"{\"latency_us\":"<<us<<",\"degraded\":"<<(sr.degraded?"true":"false")
     <<",\"shards_ok\":"<<sr.shards_ok<<",\"shards_err\":"<<sr.shards_err
     <<",\"results\":[";
    for(size_t i=0;i<sr.results.size();++i){
        if(i>0)j<<",";
        const auto& r=sr.results[i];
        j<<"{\"node_id\":"<<r.node_id<<",\"distance\":"<<std::sqrt(r.dist)<<"}";
    }
    j<<"]}"; return j.str();
}

// ── /api/insert ───────────────────────────────────────────────────────────────
//
//  Body: {"vector":[f,f,...], "vector_id":N}
//  vector_id is optional — auto-assigned if missing (uses total_vectors as id)
//
//  Returns: {"shard_id":N, "node_id":N, "vector_id":N, "latency_us":N}

std::string HttpApiServer::handle_insert(const std::string& body){
    auto vec=parse_float_array(body,"vector");
    if(vec.empty())return R"({"error":"missing vector array"})";
    if((int)vec.size()!=mgr_.cfg_dim()){
        std::ostringstream e;
        e<<"{\"error\":\"vector dim="<<vec.size()<<" but index dim="<<mgr_.cfg_dim()<<"\"}";
        return e.str();
    }

    // vector_id: use provided or auto-assign
    int64_t vid=parse_int64_field(body,"vector_id",-1);
    if(vid<0) vid=(int64_t)mgr_.total_vectors();

    auto t0=std::chrono::high_resolution_clock::now();
    auto [shard_id,node_id]=mgr_.insert((uint64_t)vid,vec.data());
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now()-t0).count();

    metrics_.record_insert((uint64_t)us);

    std::ostringstream j;
    j<<"{\"shard_id\":"<<shard_id
     <<",\"node_id\":"<<node_id
     <<",\"vector_id\":"<<vid
     <<",\"latency_us\":"<<us
     <<",\"total_vectors\":"<<mgr_.total_vectors()
     <<"}";
    return j.str();
}

// ── /api/node?id=N ────────────────────────────────────────────────────────────
//
//  Returns the HNSW graph neighbours of node N (shard 0 by default).
//  Response:
//  {
//    "node_id": 42,
//    "shard_id": 0,
//    "max_layer": 2,
//    "vector": [f, f, ...],          // first 8 floats (preview)
//    "layers": [
//      { "layer": 0, "neighbours": [3, 17, 99, ...] },
//      { "layer": 1, "neighbours": [3, 17] },
//      ...
//    ]
//  }

std::string HttpApiServer::handle_node(const std::string& qstr){
    std::string id_str=parse_query_param(qstr,"id");
    if(id_str.empty())return R"({"error":"missing id parameter e.g. /api/node?id=42"})";

    uint32_t node_id;
    try{ node_id=(uint32_t)std::stoul(id_str); }
    catch(...){ return R"({"error":"invalid id"})"; }

    // Find which shard owns this node by checking owning_shard on total count
    // Since node_ids are per-shard-local we search all shards
    // We expose it through shard 0's index for the graph inspector
    // (For a full implementation, the inspector would take shard_id too)
    uint32_t shard_id_param=0;
    std::string shard_str=parse_query_param(qstr,"shard");
    if(!shard_str.empty()) try{shard_id_param=(uint32_t)std::stoul(shard_str);}catch(...){}

    if(shard_id_param>=mgr_.shard_count())
        return R"({"error":"invalid shard_id"})";

    // Access the shard's index through the public API
    HNSWIndex* idx=mgr_.get_shard_index(shard_id_param);
    if(!idx) return R"({"error":"shard not available"})";

    if(node_id>=idx->size())
        return R"({"error":"node_id out of range"})";

    int max_layer=idx->max_layer_stored();

    std::ostringstream j;
    j<<"{\"node_id\":"<<node_id
     <<",\"shard_id\":"<<shard_id_param
     <<",\"max_layer\":"<<max_layer
     <<",\"total_nodes\":"<<idx->size();

    // First 8 floats of the vector as a preview
    const float* vec=idx->get_vector(node_id);
    j<<",\"vector_preview\":[";
    int preview=std::min(8,idx->dim());
    for(int i=0;i<preview;++i){ if(i>0)j<<","; j<<vec[i]; }
    j<<"]";

    // Neighbours at each layer
    j<<",\"layers\":[";
    for(int l=0;l<=max_layer;++l){
        if(l>0)j<<",";
        j<<"{\"layer\":"<<l<<",\"neighbours\":[";
        const auto& nbrs=idx->neighbours_at(node_id,l);
        for(size_t i=0;i<nbrs.size();++i){
            if(i>0)j<<",";
            j<<nbrs[i];
        }
        j<<"]}";
    }
    j<<"]}";
    return j.str();
}

} // namespace vecdb
