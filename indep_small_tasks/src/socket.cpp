class Socket
{
  private:
      int sock_;

  public:
    explicit Socket(int sin_family, int sin_type, int sin_protocol)
    {
        sock_ = socket(sin_family, sin_type, sin_protocol);
    }

    ~Socket()
    {
        close(sock_);
    }
};
