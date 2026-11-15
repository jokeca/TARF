#include "../src/ssd/Flash_Transaction_Queue.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

using namespace SSD_Components;

int main() {
    Flash_Transaction_Queue q;
    q.Enable_read_index();
    std::mt19937 rng(137);
    std::vector<NVM_Transaction_Flash*> allocated;
    for (unsigned step=0; step<20000; ++step) {
        const unsigned op=rng()%6;
        if (q.empty() || op<3) {
            NVM::FlashMemory::Physical_Page_Address a;
            a.ChannelID=0;a.ChipID=0;a.DieID=rng()%8;a.PlaneID=0;a.BlockID=0;a.PageID=0;
            auto* t=new NVM_Transaction_Flash(Transaction_Source_Type::USERIO,
                rng()%4 ? Transaction_Type::READ : Transaction_Type::WRITE,
                0,16384,step,step,a,nullptr,IO_Flow_Priority_Class::HIGH);
            allocated.push_back(t);
            if (op==0) q.push_front(t);
            else if(op==1)q.insert(std::next(q.begin(),rng()%(q.size()+1)),t);
            else q.push_back(t);
        } else if(op==3)q.pop_front();
        else if(op==4)q.remove(std::next(q.begin(),rng()%q.size()));
        else q.remove(*std::next(q.begin(),rng()%q.size()));
        for (unsigned die=0;die<9;++die) {
            auto expected=q.begin();
            while(expected!=q.end() && ((*expected)->Type!=Transaction_Type::READ || (*expected)->Address.DieID!=die))++expected;
            auto actual=q.end();const bool found=q.First_indexed_read(die,actual);
            assert(found==(expected!=q.end()));
            if(found)assert(actual==expected);
        }
    }
    while(!q.empty())q.pop_front();
    for(auto* p:allocated)delete p;
    std::cout << "PASS:180000 comparisons against original linear-search semantics\n";
}
