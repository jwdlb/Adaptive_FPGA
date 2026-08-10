#include "app/dashboard_server.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace market_engine::app {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

std::string dashboard_html() {
    return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Adaptive FPGA dashboard</title><style>
:root{color-scheme:dark;--bg:#081019;--card:#101c27;--line:#25384a;--ink:#d9e8f3;--muted:#829aab;--cyan:#42d6c8;--buy:#45df8b;--sell:#ff6b78;--warn:#f5c451}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 80% 0,#102d3a 0,transparent 38%),var(--bg);color:var(--ink);font:14px ui-monospace,SFMono-Regular,Consolas,monospace}header{padding:22px 4vw;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center}h1{font:600 22px system-ui;margin:0}main{padding:20px 4vw;display:grid;grid-template-columns:repeat(12,1fr);gap:14px}.card{background:color-mix(in srgb,var(--card) 92%,transparent);border:1px solid var(--line);border-radius:10px;padding:16px;grid-column:span 3}.wide{grid-column:span 6}.full{grid-column:1/-1}h2{font:600 13px system-ui;text-transform:uppercase;letter-spacing:.12em;color:var(--muted);margin:0 0 14px}.metrics{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}.metric b{display:block;font-size:20px;color:var(--cyan);margin-top:3px}.metric small{color:var(--muted)}.pill{padding:5px 10px;border:1px solid var(--line);border-radius:99px}.running{color:var(--buy)}table{width:100%;border-collapse:collapse}td,th{text-align:right;padding:5px;border-bottom:1px solid #1b2b38}td:first-child,th:first-child{text-align:left}.bars{display:grid;grid-template-columns:repeat(8,1fr);gap:5px;height:115px;align-items:end}.bar{background:linear-gradient(var(--cyan),#237a8b);min-height:2px;border-radius:3px 3px 0 0;position:relative}.bar span{position:absolute;bottom:-20px;left:0;font-size:9px;color:var(--muted)}input,button,select{background:#0b151f;color:var(--ink);border:1px solid var(--line);padding:9px;border-radius:6px}button{cursor:pointer;border-color:#397a84}form{display:flex;gap:8px;flex-wrap:wrap}.feed{height:150px;overflow:hidden;color:#91a8b8}.action{font-size:32px;font-weight:700}.BUY{color:var(--buy)}.SELL{color:var(--sell)}.job{margin-top:14px;padding:10px;border-left:4px solid var(--muted);background:#0b151f;color:var(--muted)}.job.running,.job.generating,.job.loading{border-color:var(--cyan);color:var(--cyan)}.job.queued{border-color:var(--warn);color:var(--warn)}.job.completed{border-color:var(--buy);color:var(--buy)}.job.failed{border-color:var(--sell);color:var(--sell)}.job progress{width:100%;height:8px;margin-top:8px;accent-color:currentColor}@media(max-width:900px){.card,.wide{grid-column:1/-1}}
</style></head><body><header><h1>Adaptive FPGA / GPU observatory</h1><span id="status" class="pill">connecting</span></header><main>
<section class="card wide"><h2>Controls</h2><form id="generate"><input name="output" value="data/dashboard-events.csv" aria-label="output path"><input name="events" type="number" value="100000" min="1"><input name="seed" type="number" value="42"><button>Generate CSV</button></form><br><form id="run"><input name="input" value="data/dashboard-events.csv" aria-label="input path"><input name="events" type="number" placeholder="all events" min="1"><label><input name="gpu" type="checkbox"> GPU training</label><button>Run replay</button></form><div id="command" class="job idle"><span id="command-text">Ready for a CSV generation or replay.</span><progress id="command-progress" hidden></progress></div></section>
<section class="card wide"><h2>Overview</h2><div id="overview" class="metrics"></div></section>
<section class="card"><h2>Signal</h2><div id="action" class="action">HOLD</div><p>score <span id="score">0</span></p></section>
<section class="card"><h2>Top of book</h2><table><thead><tr><th>Side</th><th>Price</th><th>Qty</th></tr></thead><tbody id="book"></tbody></table></section>
<section class="card wide"><h2>Eight RTL features (Q16.16)</h2><div id="features" class="bars"></div></section>
<section class="card wide"><h2>Model</h2><div id="model" class="metrics"></div><table><tbody id="weights"></tbody></table></section>
<section class="card wide"><h2>Performance</h2><div id="performance" class="metrics"></div></section>
<section class="card wide"><h2>Sampled event tape</h2><div id="feed" class="feed"></div></section>
</main><script>
const $=id=>document.getElementById(id), fmt=n=>Number(n||0).toLocaleString(undefined,{maximumFractionDigits:2});
function metrics(el,values){el.innerHTML=values.map(([k,v])=>`<div class="metric"><small>${k}</small><b>${v}</b></div>`).join('')}
function showActivity(a){const state=a?.state||'idle',total=Number(a?.total||0),done=Number(a?.completed||0),queued=Number(a?.queued||0);let text=a?.message||'Ready for a CSV generation or replay.';if(total)text+=` — ${fmt(done)} / ${fmt(total)} events (${fmt(done/total*100)}%)`;if(queued)text+=` — ${queued} waiting`;$('command').className='job '+state;$('command-text').textContent=text;const progress=$('command-progress');progress.hidden=!total;progress.max=total||1;progress.value=Math.min(done,total)}
function render(s){$('status').textContent=`${s.connectionState} / ${s.state} / ${s.dashboardClients} client(s)`;$('status').className='pill '+(s.state==='running'?'running':'');showActivity(s.activity);metrics($('overview'),[['events/s',fmt(s.eventsPerSecond)],['processed / errors',`${fmt(s.processedEvents)} / ${fmt(s.errorEvents)}`],['queue',`${s.queue.occupancy}/${s.queue.capacity}`],['GPU batches / updates',`${s.gpu.batches} / ${s.gpu.updates}`]]);const action=s.signal.action.toUpperCase();$('action').textContent=action;$('action').className='action '+action;$('score').textContent=`${s.signal.scoreQ16} (${fmt(s.signal.scoreQ16/65536)})`;const bid=s.book.bids.find(x=>x.quantity),ask=s.book.asks.find(x=>x.quantity);$('book').innerHTML=[['BID',bid],['ASK',ask]].map(([n,x])=>`<tr><td>${n}</td><td>${x?.priceTicks??'-'}</td><td>${x?.quantity??'-'}</td></tr>`).join('');const vals=s.featuresQ16,max=Math.max(1,...vals.map(Math.abs));$('features').innerHTML=vals.map((v,i)=>`<div class="bar" title="${v}" style="height:${Math.max(2,Math.abs(v)/max*95)}%"><span>f${i}</span></div>`).join('');const updated=s.gpu.latestUpdateUnixMs?new Date(s.gpu.latestUpdateUnixMs).toLocaleTimeString():'—';metrics($('model'),[['version',s.model.version],['buy / sell',`${s.model.buyThresholdQ16} / ${s.model.sellThresholdQ16}`],['loss / accuracy',`${fmt(s.gpu.loss)} / ${fmt(s.gpu.accuracy*100)}%`],['latest update',updated]]);$('weights').innerHTML=s.model.weightsQ16.map((w,i)=>`<tr><td>w${i}</td><td>${w}</td><td>${fmt(w/65536)}</td></tr>`).join('');metrics($('performance'),[['RTL cycles/event',fmt(s.performance.rtlCyclesPerEvent)],['GPU training ms',fmt(s.performance.gpuTrainingMs)],['update latency ms',fmt(s.performance.updateLatencyMs)],['clients',s.dashboardClients]]);$('feed').innerHTML=[...s.recentEvents].reverse().map(e=>`<div>${e.timestampNs} &nbsp; ${e.type.padEnd(6)} ${e.side.padEnd(3)} ${e.priceTicks}@${e.quantity}</div>`).join('')}
function connect(){const ws=new WebSocket(`ws://${location.host}/ws`);ws.onopen=()=>{$('status').textContent='connected'};ws.onmessage=e=>render(JSON.parse(e.data));ws.onclose=()=>{ $('status').textContent='reconnecting';setTimeout(connect,1000)}}connect();
async function command(action,form){const data=Object.fromEntries(new FormData(form));data.action=action;if('gpu'in data)data.gpu=true;for(const k of ['events','seed']){if(data[k])data[k]=Number(data[k]);else delete data[k]}try{const r=await fetch('/api/control',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(data)});const reply=await r.json();if(!r.ok)throw new Error(reply.error||'Command was rejected');showActivity({state:'queued',message:`${action==='generate'?'CSV generation':'Replay'} queued`,queued:reply.queued})}catch(error){showActivity({state:'failed',message:error.message})}}
$('generate').onsubmit=e=>{e.preventDefault();command('generate',e.target)};$('run').onsubmit=e=>{e.preventDefault();command('run',e.target)};
</script></body></html>)HTML";
}

class DashboardServer::Impl {
public:
    Impl(DashboardServerOptions options, std::shared_ptr<DashboardSnapshotStore> snapshots,
         ControlHandler control)
        : options_(std::move(options)), snapshots_(std::move(snapshots)), control_(std::move(control)),
          acceptor_(ioc_) {
        if (!snapshots_) throw std::invalid_argument("dashboard snapshot store is required");
        if (options_.update_hz == 0U) throw std::invalid_argument("dashboard update rate must be positive");
    }

    struct WebSocketSession : std::enable_shared_from_this<WebSocketSession> {
        WebSocketSession(tcp::socket socket, Impl& owner)
            : ws(std::move(socket)), timer(ws.get_executor()), owner(owner) {}
        void start(http::request<http::string_body> request) {
            ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
            ws.async_accept(request, [self=shared_from_this()](beast::error_code ec) {
                if (ec) return self->finish();
                self->counted = true; self->owner.clients_.fetch_add(1, std::memory_order_relaxed);
                self->read_control();
                self->send_latest();
            });
        }
        void read_control() {
            ws.async_read(inbound, [self=shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->finish();
                self->inbound.consume(self->inbound.size());
                self->read_control();
            });
        }
        void send_latest() {
            DashboardSnapshot view = *owner.snapshots_->latest();
            view.dashboard_clients = owner.clients_.load(std::memory_order_relaxed);
            view.connection_state = view.dashboard_clients == 0U ? "listening" : "connected";
            outbound = dashboard_snapshot_json(view);
            ws.text(true);
            ws.async_write(asio::buffer(outbound), [self=shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->finish();
                self->timer.expires_after(std::chrono::milliseconds(1000U / self->owner.options_.update_hz));
                self->timer.async_wait([self](beast::error_code timer_ec) { if (!timer_ec) self->send_latest(); });
            });
        }
        void finish() {
            beast::error_code ignored; timer.cancel(ignored);
            if (counted) { counted=false; owner.clients_.fetch_sub(1, std::memory_order_relaxed); }
        }
        websocket::stream<beast::tcp_stream> ws;
        asio::steady_timer timer;
        beast::flat_buffer inbound;
        Impl& owner;
        std::string outbound;
        bool counted{};
    };

    struct HttpSession : std::enable_shared_from_this<HttpSession> {
        HttpSession(tcp::socket socket, Impl& owner) : stream(std::move(socket)), owner(owner) {}
        void start() { read(); }
        void read() {
            parser.emplace(); parser->body_limit(64U * 1024U);
            http::async_read(stream, buffer, *parser, [self=shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->bad_request(ec);
                auto request = self->parser->release();
                if (websocket::is_upgrade(request) && request.target() == "/ws") {
                    std::make_shared<WebSocketSession>(self->stream.release_socket(), self->owner)->start(std::move(request));
                    return;
                }
                self->respond(std::move(request));
            });
        }
        void bad_request(beast::error_code ec) {
            if (ec == http::error::end_of_stream || ec == asio::error::operation_aborted) return;
            send(http::status::bad_request, "text/plain", "malformed HTTP request\n");
        }
        void respond(http::request<http::string_body> request) {
            if (request.method() == http::verb::get && (request.target() == "/" || request.target() == "/index.html")) {
                return send(http::status::ok, "text/html; charset=utf-8", dashboard_html());
            }
            if (request.method() == http::verb::get && request.target() == "/health") {
                return send(http::status::ok, "application/json", "{\"ok\":true}");
            }
            if (request.method() == http::verb::post && request.target() == "/api/control") {
                if (!owner.control_) return send(http::status::service_unavailable, "application/json", "{\"error\":\"controls disabled\"}");
                try { return send(http::status::accepted, "application/json", owner.control_(request.body())); }
                catch (const std::exception& error) { return send(http::status::bad_request, "application/json", std::string("{\"error\":\"") + escape(error.what()) + "\"}"); }
            }
            send(http::status::not_found, "text/plain", "not found\n");
        }
        static std::string escape(std::string_view text) {
            std::string out; out.reserve(text.size());
            for (char c : text) { if (c == '"' || c == '\\') out.push_back('\\'); if (c >= 0x20) out.push_back(c); }
            return out;
        }
        void send(http::status status, std::string_view type, std::string body) {
            response.emplace(status, 11); response->set(http::field::server, "adaptive-fpga-dashboard");
            response->set(http::field::content_type, std::string(type)); response->set(http::field::cache_control, "no-store");
            response->body() = std::move(body); response->prepare_payload(); response->keep_alive(false);
            http::async_write(stream, *response, [self=shared_from_this()](beast::error_code, std::size_t) {
                beast::error_code ignored; self->stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
            });
        }
        beast::tcp_stream stream; beast::flat_buffer buffer; Impl& owner;
        std::optional<http::request_parser<http::string_body>> parser;
        std::optional<http::response<http::string_body>> response;
    };

    void start() {
        if (running_.exchange(true)) return;
        beast::error_code ec;
        const auto address = asio::ip::make_address(options_.bind_address, ec);
        if (ec) { running_=false; throw std::runtime_error("invalid dashboard bind address: " + options_.bind_address); }
        acceptor_.open(address.is_v6() ? tcp::v6() : tcp::v4(), ec);
        if (!ec) acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        if (!ec) acceptor_.bind({address, options_.port}, ec);
        if (!ec) acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) { running_=false; throw std::runtime_error("cannot start dashboard server: " + ec.message()); }
        port_.store(acceptor_.local_endpoint().port(), std::memory_order_relaxed);
        accept();
        thread_ = std::thread([this] { ioc_.run(); });
    }
    void accept() {
        acceptor_.async_accept(asio::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<HttpSession>(std::move(socket), *this)->start();
            if (running_.load(std::memory_order_relaxed)) accept();
        });
    }
    void stop() noexcept {
        if (!running_.exchange(false)) return;
        auto closed = std::make_shared<std::promise<void>>();
        auto future = closed->get_future();
        asio::post(ioc_, [this, closed] {
            beast::error_code ignored; acceptor_.cancel(ignored); acceptor_.close(ignored); closed->set_value();
        });
        future.wait();
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    DashboardServerOptions options_; std::shared_ptr<DashboardSnapshotStore> snapshots_; ControlHandler control_;
    asio::io_context ioc_{1}; tcp::acceptor acceptor_; std::thread thread_;
    std::atomic_bool running_{}; std::atomic<std::uint16_t> port_{}; std::atomic<std::size_t> clients_{};
};

DashboardServer::DashboardServer(DashboardServerOptions options, std::shared_ptr<DashboardSnapshotStore> snapshots,
                                 ControlHandler control_handler)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(snapshots), std::move(control_handler))) {}
DashboardServer::~DashboardServer() { stop(); }
void DashboardServer::start() { impl_->start(); }
void DashboardServer::stop() noexcept { impl_->stop(); }
bool DashboardServer::running() const noexcept { return impl_->running_.load(std::memory_order_relaxed); }
std::uint16_t DashboardServer::local_port() const noexcept { return impl_->port_.load(std::memory_order_relaxed); }
std::size_t DashboardServer::client_count() const noexcept { return impl_->clients_.load(std::memory_order_relaxed); }

}  // namespace market_engine::app
