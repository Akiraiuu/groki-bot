#include "esp_tls_scope_resolver.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
static unsigned checks;
static void check(bool value,const char*why){++checks;if(!value){std::cerr<<why<<'\n';std::abort();}}
struct Callback {void(*fn)(void*);void*arg;};
struct Dns {dns_found_callback fn;void*arg;};
static std::mutex mutex;static std::condition_variable cv;static std::deque<Callback> queue;static std::deque<Dns> dns;
static std::atomic<int64_t> clock_us{1000};static std::atomic<bool> permission{true};
static std::atomic<bool> pause_poll{false};
static std::atomic<unsigned> posts{0},queries{0},cancels{0};static unsigned reject_post=0;static int dns_mode=0;
extern "C" int64_t esp_timer_get_time(){return clock_us;}
extern "C" void vTaskDelay(unsigned){while(pause_poll.load())std::this_thread::yield();std::this_thread::yield();}
extern "C" err_t tcpip_try_callback(void(*fn)(void*),void*arg){const auto n=++posts;if(n==reject_post)return ERR_MEM;{std::lock_guard hold{mutex};queue.push_back({fn,arg});}cv.notify_all();return ERR_OK;}
extern "C" err_t dns_gethostbyname_addrtype(const char*,ip_addr_t*addr,dns_found_callback fn,void*arg,u8_t){++queries;if(dns_mode==0){addr->addr=htonl(INADDR_LOOPBACK);return ERR_OK;}if(dns_mode==2)return ERR_MEM;dns.push_back({fn,arg});return ERR_INPROGRESS;}
extern "C" int dns_cancel_request(dns_found_callback fn,void*arg){++cancels;for(auto i=dns.begin();i!=dns.end();++i)if(i->fn==fn&&i->arg==arg){dns.erase(i);return 1;}return 0;}
static void wait_queue(){std::unique_lock hold{mutex};check(cv.wait_for(hold,std::chrono::seconds(2),[]{return !queue.empty();}),"queued callback did not arrive");}
static void run_one(){Callback c;{std::lock_guard hold{mutex};check(!queue.empty(),"one queued callback exists");c=queue.front();queue.pop_front();}c.fn(c.arg);}
static void deliver(bool success){check(!dns.empty(),"one DNS callback exists");const auto c=dns.front();dns.pop_front();ip_addr_t ip{htonl(INADDR_LOOPBACK)};c.fn("synthetic.invalid",success?&ip:nullptr,c.arg);}
static void reset(int mode=0){check(queue.empty()&&dns.empty(),"all old callback references retired");clock_us=1000;permission=true;posts=queries=cancels=0;reject_post=0;dns_mode=mode;}
static void init(esp_tls_scope&s){check(tls_scope_init(&s,7,1000,60000000),"fresh scope");check(tls_scope_set_permission(&s,7,+[](void*){return permission.load();},nullptr),"permission context");}
static auto resolve(esp_tls_scope&s,sockaddr_storage&out){return std::async(std::launch::async,[&s,&out]{return esp_tls_scope_resolve("synthetic.invalid",17,AF_INET,&s,7,&out);});}
template<class T>static T ready(std::future<T>&f){check(f.wait_for(std::chrono::seconds(2))==std::future_status::ready,"resolver caller did not retire");return f.get();}
static void close_scope(esp_tls_scope&s){int fd=99;check(tls_scope_close_claim(&s,7,&fd)==TLS_SCOPE_OK&&fd==-1,"all DNS refs retired before empty close");check(tls_scope_close_finish(&s,7,fd,0)==TLS_SCOPE_OK&&tls_scope_release(&s,7),"scope released");}
int main(){
    reset();{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();run_one();check(ready(f)==ESP_OK&&queries==1&&reinterpret_cast<sockaddr_in*>(&out)->sin_addr.s_addr==htonl(INADDR_LOOPBACK),"cached response copied before reference retirement");close_scope(s);}
    reset(1);{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();run_one();deliver(true);check(ready(f)==ESP_OK&&cancels==0,"normal asynchronous result returns without a cancellation or repeated query");close_scope(s);}
    reset();{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();permission=false;check(ready(f)!=ESP_OK&&queries==0,"cancelled queued start returns without a DNS request");int fd;check(tls_scope_close_claim(&s,7,&fd)==TLS_SCOPE_PENDING,"queued callback still pins owner");run_one();check(queries==0&&cancels==0,"late start callback cannot query after cancellation");close_scope(s);}
    reset(1);{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();run_one();check(queries==1&&dns.size()==1,"one asynchronous DNS admitted");permission=false;check(ready(f)!=ESP_OK,"active resolver caller cancels");wait_queue();int fd;check(tls_scope_close_claim(&s,7,&fd)==TLS_SCOPE_PENDING,"cancel submission is not callback retirement");run_one();check(cancels==1&&dns.empty(),"one core cancellation detaches the exact DNS consumer");close_scope(s);}
    reset(1);{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();run_one();permission=false;ready(f);wait_queue();deliver(true);check(tls_scope_observe(&s,7,1000).io_count==1,"found callback cannot free queued cancellation context");run_one();check(cancels==0,"completed callback is not cancelled again");close_scope(s);}
    reset(1);{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();run_one();reject_post=2;permission=false;ready(f);check(posts==2&&queue.empty()&&dns.size()==1&&tls_scope_observe(&s,7,1000).io_count==1,"full cancellation mailbox retains real callback, with no automatic repost");deliver(false);check(posts==2,"timeout callback did not repost cancel");close_scope(s);}
    reset();{esp_tls_scope s{};init(s);sockaddr_storage out{};reject_post=1;auto f=resolve(s,out);check(ready(f)!=ESP_OK&&posts==1&&queries==0,"full start mailbox fails once without query");close_scope(s);}
    reset(2);{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();run_one();check(ready(f)!=ESP_OK&&queries==1&&dns.empty(),"synchronous resolver failure releases asynchronous reservation");close_scope(s);}
    reset();{esp_tls_scope s{};init(s);sockaddr_storage out{};auto f=resolve(s,out);wait_queue();clock_us=60000000;check(ready(f)!=ESP_OK,"original scope end cancels queued lookup");run_one();check(queries==0,"expired callback never starts a query");close_scope(s);}
    reset();{esp_tls_scope s{};init(s);sockaddr_storage out{};pause_poll=true;
    auto f=std::async(std::launch::async,[&]{tls_scope_io budget{};if(tls_scope_io_begin_timed(&s,7,1000,100,&budget)!=TLS_SCOPE_OK)std::abort();const auto result=esp_tls_scope_resolve("synthetic.invalid",17,AF_INET,&s,7,&out);tls_scope_io_end(&budget);return result;});
    wait_queue();clock_us=101001;run_one();check(queries==0,"TCP/IP callback carries caller's shorter deadline even before caller can cancel it");pause_poll=false;check(ready(f)!=ESP_OK,"expired per-call lookup retires without a query");close_scope(s);}
    reset();{esp_tls_scope s{};init(s);sockaddr_storage out{};check(esp_tls_scope_resolve("a",1,AF_INET6,&s,7,&out)==ESP_ERR_INVALID_ARG&&posts==0,"IPv6 unsupported scope is explicit and creates no work");check(esp_tls_scope_resolve("bad/name",8,AF_INET,&s,7,&out)==ESP_ERR_INVALID_ARG,"non-host input rejected");close_scope(s);}
    reset();{esp_tls_scope first{},second{},third{};init(first);init(second);init(third);sockaddr_storage one{},two{},three{};auto a=resolve(first,one);auto b=resolve(second,two);
    {std::unique_lock hold{mutex};check(cv.wait_for(hold,std::chrono::seconds(2),[]{return queue.size()==2;}),"two admitted lookup jobs queued");}
    auto c=resolve(third,three);check(ready(c)==ESP_ERR_NO_MEM&&posts==2,"fixed pool rejects a third lookup without queued work");
    permission=false;check(ready(a)!=ESP_OK&&ready(b)!=ESP_OK,"both admitted callers cancel");run_one();run_one();check(queries==0,"pool cancellation never begins late queries");close_scope(first);close_scope(second);close_scope(third);}
    std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<",\"actual_resolver_c\":true,\"dns_and_tcpip_executor_stubbed\":true,\"network_access\":false}\n";
}
