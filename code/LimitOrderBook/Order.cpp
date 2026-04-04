#include "Order.h"

Order::Order(Id id, Side side, Price price, Quantity initQ):
    id(id),
    side(side),
    price(price),
    initQ(initQ),
    leftQ(initQ),
    status(OrderStatus::WAITING_ACK)
{};
  
Id Order::getId() const {return id;}
Side Order::getSide() const {return side;}
Price Order::getPrice() const {return price;}
Quantity Order::getInitialQuantity() const {return initQ;}
Quantity Order::getRemainingQuantity() const {return leftQ;}
OrderStatus Order::getOrderStatus() const {return status;}


void Order::fill(const Quantity q) {
  if (q > getRemainingQuantity()) {
    throw std::invalid_argument("Order ('" + std::to_string(id) + "') cannot be filled for more than its current remaining quantity");
  }

  leftQ -= q;
  if (leftQ == 0) {
      status = OrderStatus::COMPLETE_FILL;
  }
  else if (leftQ < getInitialQuantity()) {
      status = OrderStatus::PARTIAL_FILL;
  }
}