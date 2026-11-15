#include "Flash_Transaction_Queue.h"

namespace SSD_Components
{
	Flash_Transaction_Queue::Flash_Transaction_Queue() {}

	Flash_Transaction_Queue::Flash_Transaction_Queue(std::string id) : id(id)
	{
	}

	void Flash_Transaction_Queue::Set_id(std::string id)
	{
		this->id = id;
	}

	void Flash_Transaction_Queue::push_back(NVM_Transaction_Flash* const& transaction)
	{
		RequestQueueProbe.EnqueueRequest(transaction);
		list<NVM_Transaction_Flash*>::push_back(transaction);
        if (read_index_enabled_ && transaction->Type == Transaction_Type::READ
            && first_read_by_die_.count(transaction->Address.DieID) == 0)
            first_read_by_die_[transaction->Address.DieID] = std::prev(end());
	}

	void Flash_Transaction_Queue::push_front(NVM_Transaction_Flash* const& transaction)
	{
		RequestQueueProbe.EnqueueRequest(transaction);
		list<NVM_Transaction_Flash*>::push_front(transaction);
        if (read_index_enabled_ && transaction->Type == Transaction_Type::READ)
            first_read_by_die_[transaction->Address.DieID] = begin();
	}

	std::list<NVM_Transaction_Flash*>::iterator Flash_Transaction_Queue::insert(list<NVM_Transaction_Flash*>::iterator position, NVM_Transaction_Flash* const& transaction)
	{
		RequestQueueProbe.EnqueueRequest(transaction);
		auto result = list<NVM_Transaction_Flash*>::insert(position, transaction);
        if (read_index_enabled_) Rebuild_read_index();
        return result;
	}

	void Flash_Transaction_Queue::remove(NVM_Transaction_Flash* const& transaction)
	{
		RequestQueueProbe.DequeueRequest(transaction);
		list<NVM_Transaction_Flash*>::remove(transaction);
        if (read_index_enabled_) Rebuild_read_index();
	}

	void Flash_Transaction_Queue::remove(std::list<NVM_Transaction_Flash*>::iterator const& itr_pos)
	{
		RequestQueueProbe.DequeueRequest(*itr_pos);
		Before_indexed_erase(itr_pos);
		list<NVM_Transaction_Flash*>::erase(itr_pos);
	}

	void Flash_Transaction_Queue::pop_front()
	{
		RequestQueueProbe.DequeueRequest(this->front());
		Before_indexed_erase(begin());
		list<NVM_Transaction_Flash*>::pop_front();
	}

    void Flash_Transaction_Queue::Enable_read_index() {
        read_index_enabled_ = true;
        Rebuild_read_index();
    }
    void Flash_Transaction_Queue::Rebuild_read_index() {
        first_read_by_die_.clear();
        for (auto it = begin(); it != end(); ++it)
            if ((*it)->Type == Transaction_Type::READ && first_read_by_die_.count((*it)->Address.DieID) == 0)
                first_read_by_die_[(*it)->Address.DieID] = it;
    }
    bool Flash_Transaction_Queue::First_indexed_read(unsigned die, std::list<NVM_Transaction_Flash*>::iterator& result) const {
        const auto found = first_read_by_die_.find(die);
        if (found == first_read_by_die_.end()) return false;
        result = found->second;
        return true;
    }
    void Flash_Transaction_Queue::Before_indexed_erase(std::list<NVM_Transaction_Flash*>::iterator position) {
        if (!read_index_enabled_ || (*position)->Type != Transaction_Type::READ) return;
        const unsigned die = (*position)->Address.DieID;
        auto found = first_read_by_die_.find(die);
        if (found == first_read_by_die_.end() || found->second != position) return;
        for (auto next = std::next(position); next != end(); ++next) {
            if ((*next)->Type == Transaction_Type::READ && (*next)->Address.DieID == die) {
                found->second = next;
                return;
            }
        }
        first_read_by_die_.erase(found);
    }

	void Flash_Transaction_Queue::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter)
	{
		// std::string tmp = name_prefix;
		// //xmlwriter.Write_start_element_tag(tmp);
		// xmlwriter.Write_open_tag(tmp);


		// std::string attr = "Name";
		// std::string val = id;
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "No_Of_Transactions_Enqueued";
		// val = std::to_string(RequestQueueProbe.NRequests());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "No_Of_Transactions_Dequeued";
		// val = std::to_string(RequestQueueProbe.NDepartures());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "Avg_Queue_Length";
		// val = std::to_string(RequestQueueProbe.AvgQueueLength());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "Max_Queue_Length";
		// val = std::to_string(RequestQueueProbe.MaxQueueLength());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "STDev_Queue_Length";
		// val = std::to_string(RequestQueueProbe.STDevQueueLength());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "Avg_Transaction_Waiting_Time";
		// val = std::to_string(RequestQueueProbe.AvgWaitingTime());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// attr = "Max_Transaction_Waiting_Time";
		// val = std::to_string(RequestQueueProbe.MaxWaitingTime());
		// xmlwriter.Write_attribute_string(attr, val);
		// //xmlwriter.Write_attribute_string_inline(attr, val);

		// //xmlwriter.Write_end_element_tag();
		// xmlwriter.Write_close_tag();
	}
}
