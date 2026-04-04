#include <iostream> 

enum class Side {
  BUY, 
  SELL
};

enum class OrderStatus {
  WAITING_ACK,
  PARTIAL_FILL,
  COMPLETE_FILL
};

using Id = std::uint64_t;
using Price = std::uint32_t;
using Quantity = std::uint32_t;

class Order {
  Id id;
  Side side; 
  Price price;
  Quantity initQ; // initial quantity
  Quantity leftQ; // remaining quantity
  OrderStatus status;

public: 
  Order(Id id, Side side, Price price, Quantity initQ);
  
  // read only 
  Id getId() const;
  Side getSide() const;
  Price getPrice() const;
  Quantity getInitialQuantity() const;
  Quantity getRemainingQuantity() const;
  OrderStatus getOrderStatus() const;

  // rw
  void fill(const Quantity q);

  /* TODO: move to order book
  void cancel();
  void modify(int newQuantity); // equiv to cancel + fill
  void submit(); 
  */
};
